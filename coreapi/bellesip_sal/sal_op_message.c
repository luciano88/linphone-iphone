/*
linphone
Copyright (C) 2012  Belledonne Communications, Grenoble, France

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/
#include "sal_impl.h"

#include "linphonecore.h"
#include "private.h"
#include "lime.h"
#include <libxml/xmlwriter.h>

static void process_error( SalOp* op) {
	if (op->dir == SalOpDirOutgoing) {
		op->base.root->callbacks.text_delivery_update(op, SalTextDeliveryFailed);
	} else {
		ms_warning("unexpected io error for incoming message on op [%p]",op);
	}
	op->state=SalOpStateTerminated;

}

static void process_io_error(void *user_ctx, const belle_sip_io_error_event_t *event){
	SalOp* op = (SalOp*)user_ctx;
	sal_error_info_set(&op->error_info,SalReasonIOError,503,"IO Error",NULL);
	process_error(op);
}
static void process_timeout(void *user_ctx, const belle_sip_timeout_event_t *event) {
	SalOp* op=(SalOp*)user_ctx;
	sal_error_info_set(&op->error_info,SalReasonRequestTimeout,408,"Request timeout",NULL);
	process_error(op);

}
static void process_response_event(void *op_base, const belle_sip_response_event_t *event){
	SalOp* op = (SalOp*)op_base;
	int code = belle_sip_response_get_status_code(belle_sip_response_event_get_response(event));
	SalTextDeliveryStatus status;
	sal_op_set_error_info_from_response(op,belle_sip_response_event_get_response(event));
	
	if (code>=100 && code <200)
		status=SalTextDeliveryInProgress;
	else if (code>=200 && code <300)
		status=SalTextDeliveryDone;
	else
		status=SalTextDeliveryFailed;
	
	op->base.root->callbacks.text_delivery_update(op,status);
}

static bool_t is_rcs_filetransfer(belle_sip_header_content_type_t* content_type) {
	return (strcmp("application",belle_sip_header_content_type_get_type(content_type))==0)
			&&	((strcmp("vnd.gsma.rcs-ft-http+xml",belle_sip_header_content_type_get_subtype(content_type))==0) || (strcmp("cipher.vnd.gsma.rcs-ft-http+xml",belle_sip_header_content_type_get_subtype(content_type))==0));
}

static bool_t is_plain_text(belle_sip_header_content_type_t* content_type) {
	return strcmp("text",belle_sip_header_content_type_get_type(content_type))==0
			&&	strcmp("plain",belle_sip_header_content_type_get_subtype(content_type))==0;
}

static bool_t is_cipher_xml(belle_sip_header_content_type_t* content_type) {
	return (strcmp("xml",belle_sip_header_content_type_get_type(content_type))==0
			&&	strcmp("cipher",belle_sip_header_content_type_get_subtype(content_type))==0)

		|| (strcmp("application",belle_sip_header_content_type_get_type(content_type))==0
			&&	strcmp("cipher.vnd.gsma.rcs-ft-http+xml",belle_sip_header_content_type_get_subtype(content_type))==0);
}
static bool_t is_external_body(belle_sip_header_content_type_t* content_type) {
	return strcmp("message",belle_sip_header_content_type_get_type(content_type))==0
			&&	strcmp("external-body",belle_sip_header_content_type_get_subtype(content_type))==0;
}
static bool_t is_im_iscomposing(belle_sip_header_content_type_t* content_type) {
	return strcmp("application",belle_sip_header_content_type_get_type(content_type))==0
			&&	strcmp("im-iscomposing+xml",belle_sip_header_content_type_get_subtype(content_type))==0;
}

static void add_message_accept(belle_sip_message_t *msg){
	belle_sip_message_add_header(msg,belle_sip_header_create("Accept","text/plain, message/external-body, application/im-iscomposing+xml, xml/cipher, application/vnd.gsma.rcs-ft-http+xml, application/cipher.vnd.gsma.rcs-ft-http+xml"));
}

void sal_process_incoming_message(SalOp *op,const belle_sip_request_event_t *event){
	belle_sip_request_t* req = belle_sip_request_event_get_request(event);
	belle_sip_server_transaction_t* server_transaction = belle_sip_provider_create_server_transaction(op->base.root->prov,req);
	belle_sip_header_address_t* address;
	belle_sip_header_from_t* from_header;
	belle_sip_header_content_type_t* content_type;
	belle_sip_response_t* resp;
	belle_sip_header_call_id_t* call_id = belle_sip_message_get_header_by_type(req,belle_sip_header_call_id_t);
	belle_sip_header_cseq_t* cseq = belle_sip_message_get_header_by_type(req,belle_sip_header_cseq_t);
	belle_sip_header_date_t *date=belle_sip_message_get_header_by_type(req,belle_sip_header_date_t);
	char* from;
	bool_t plain_text=FALSE;
	bool_t external_body=FALSE;
	bool_t cipher_xml=FALSE;
	bool_t rcs_filetransfer=FALSE;
	uint8_t *decryptedMessage = NULL;

	from_header=belle_sip_message_get_header_by_type(BELLE_SIP_MESSAGE(req),belle_sip_header_from_t);
	content_type=belle_sip_message_get_header_by_type(BELLE_SIP_MESSAGE(req),belle_sip_header_content_type_t);
	/* check if we have a xml/cipher message to be decrypted */
	if (content_type && (cipher_xml=is_cipher_xml(content_type))) {
		/* access the zrtp cache to get keys needed to decipher the message */
		LinphoneCore *lc=(LinphoneCore *)sal_get_user_pointer(sal_op_get_sal(op));
		FILE *CACHEFD = fopen(lc->zrtp_secrets_cache, "r+");
		if (CACHEFD == NULL) {
			ms_warning("Unable to access ZRTP ZID cache to decrypt message");
		} else {
			int cacheSize;
			uint8_t *cacheString;
			int retval;
			xmlDocPtr cacheXml;

			fseek(CACHEFD, 0L, SEEK_END);  /* Position to end of file */
			cacheSize = ftell(CACHEFD);     /* Get file length */
			rewind(CACHEFD);               /* Back to start of file */
			cacheString = (uint8_t *)malloc(cacheSize*sizeof(uint8_t)+1); /* string must be null terminated */
			fread(cacheString, 1, cacheSize, CACHEFD);
			cacheString[cacheSize] = '\0';
			cacheSize += 1;
			fclose(CACHEFD);
			cacheXml = xmlParseDoc(cacheString);
			free(cacheString);
			retval = lime_decryptMultipartMessage(cacheXml, (uint8_t *)belle_sip_message_get_body(BELLE_SIP_MESSAGE(req)), &decryptedMessage);
			if (retval != 0) {
				ms_warning("Unable to decrypt message, reason : %s - op [%p]", lime_error_code_to_string(retval), op);
				free(decryptedMessage);
				xmlFreeDoc(cacheXml);
				resp = belle_sip_response_create_from_request(req,488);
				belle_sip_server_transaction_send_response(server_transaction,resp);
				return;
			} else {
				/* dump updated cache to a string */
				xmlChar *xmlStringOutput;
				int xmlStringLength;
				xmlDocDumpFormatMemoryEnc(cacheXml, &xmlStringOutput, &xmlStringLength, "UTF-8", 0);
				/* write it to the cache file */
				CACHEFD = fopen(lc->zrtp_secrets_cache, "w+");
				fwrite(xmlStringOutput, 1, xmlStringLength, CACHEFD);
				xmlFree(xmlStringOutput);
				fclose(CACHEFD);
			}

			xmlFreeDoc(cacheXml);
		}

	}

	rcs_filetransfer=is_rcs_filetransfer(content_type);
	if (content_type && ((plain_text=is_plain_text(content_type))
						|| (external_body=is_external_body(content_type))
						|| (decryptedMessage!=NULL) 
						|| rcs_filetransfer)) {
		SalMessage salmsg;
		char message_id[256]={0};
	
		if (op->pending_server_trans) belle_sip_object_unref(op->pending_server_trans);
		op->pending_server_trans=server_transaction;
		belle_sip_object_ref(op->pending_server_trans);
	
		address=belle_sip_header_address_create(belle_sip_header_address_get_displayname(BELLE_SIP_HEADER_ADDRESS(from_header))
				,belle_sip_header_address_get_uri(BELLE_SIP_HEADER_ADDRESS(from_header)));
		from=belle_sip_object_to_string(BELLE_SIP_OBJECT(address));
		snprintf(message_id,sizeof(message_id)-1,"%s%i"
				,belle_sip_header_call_id_get_call_id(call_id)
				,belle_sip_header_cseq_get_seq_number(cseq));
		salmsg.from=from;
		/* if we just deciphered a message, use the deciphered part(which can be a rcs xml body pointing to the file to retreive from server)*/
		if (cipher_xml) {
			salmsg.text = (char *)decryptedMessage;
		} else { /* message body wasn't ciphered */
			salmsg.text=(plain_text||rcs_filetransfer)?belle_sip_message_get_body(BELLE_SIP_MESSAGE(req)):NULL;
		}
		salmsg.url=NULL;
		salmsg.content_type = NULL;
		if (rcs_filetransfer) { /* if we have a rcs file transfer, set the type, message body (stored in salmsg.text) contains all needed information to retrieve the file */
			salmsg.content_type = "application/vnd.gsma.rcs-ft-http+xml";
		}
		if (external_body && belle_sip_parameters_get_parameter(BELLE_SIP_PARAMETERS(content_type),"URL")) {
			size_t url_length=strlen(belle_sip_parameters_get_parameter(BELLE_SIP_PARAMETERS(content_type),"URL"));
			salmsg.url = ms_strdup(belle_sip_parameters_get_parameter(BELLE_SIP_PARAMETERS(content_type),"URL")+1); /* skip first "*/
			((char*)salmsg.url)[url_length-2]='\0'; /*remove trailing "*/
		}
		salmsg.message_id=message_id;
		salmsg.time=date ? belle_sip_header_date_get_time(date) : time(NULL);
		op->base.root->callbacks.text_received(op,&salmsg);
		belle_sip_object_unref(address);
		belle_sip_free(from);
		if (salmsg.url) ms_free((char*)salmsg.url);
	} else if (content_type && is_im_iscomposing(content_type)) {
		SalIsComposing saliscomposing;
		address=belle_sip_header_address_create(belle_sip_header_address_get_displayname(BELLE_SIP_HEADER_ADDRESS(from_header))
				,belle_sip_header_address_get_uri(BELLE_SIP_HEADER_ADDRESS(from_header)));
		from=belle_sip_object_to_string(BELLE_SIP_OBJECT(address));
		saliscomposing.from=from;
		saliscomposing.text=belle_sip_message_get_body(BELLE_SIP_MESSAGE(req));
		op->base.root->callbacks.is_composing_received(op,&saliscomposing);
		resp = belle_sip_response_create_from_request(req,200);
		belle_sip_server_transaction_send_response(server_transaction,resp);
		belle_sip_object_unref(address);
		belle_sip_free(from);
	} else {
		ms_error("Unsupported MESSAGE (content-type not recognized)");
		resp = belle_sip_response_create_from_request(req,415);
		add_message_accept((belle_sip_message_t*)resp);
		belle_sip_server_transaction_send_response(server_transaction,resp);
		return;
	}
}

static void process_request_event(void *op_base, const belle_sip_request_event_t *event) {
	SalOp* op = (SalOp*)op_base;
	sal_process_incoming_message(op,event);
}

int sal_message_send(SalOp *op, const char *from, const char *to, const char* content_type, const char *msg, const char *peer_uri){
	belle_sip_request_t* req;
	char content_type_raw[256];
	size_t content_length = msg?strlen(msg):0;
	time_t curtime=time(NULL);
	uint8_t *multipartEncryptedMessage = NULL;
	
	if (op->dialog){
		/*for SIP MESSAGE that are sent in call's dialog*/
		req=belle_sip_dialog_create_queued_request(op->dialog,"MESSAGE");
	}else{
		sal_op_message_fill_cbs(op);
		if (from)
			sal_op_set_from(op,from);
		if (to)
			sal_op_set_to(op,to);
		op->dir=SalOpDirOutgoing;

		req=sal_op_build_request(op,"MESSAGE");
		if (sal_op_get_contact_address(op)){
			belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),BELLE_SIP_HEADER(sal_op_create_contact(op)));
		}
	}

	/* shall we try to encrypt the message?*/
	if ((strcmp(content_type, "xml/cipher") == 0) || ((strcmp(content_type, "application/cipher.vnd.gsma.rcs-ft-http+xml") == 0))) {
		/* access the zrtp cache to get keys needed to cipher the message */
		LinphoneCore *lc=(LinphoneCore *)sal_get_user_pointer(sal_op_get_sal(op));
		FILE *CACHEFD = fopen(lc->zrtp_secrets_cache, "r+");
		if (CACHEFD == NULL) {
			ms_warning("Unable to access ZRTP ZID cache to encrypt message");
			sal_error_info_set(&op->error_info, SalReasonNotAcceptable, 488, "Unable to encrypt IM", NULL);
			op->base.root->callbacks.text_delivery_update(op,SalTextDeliveryFailed);
			return 0;
		} else {
			int cacheSize;
			uint8_t *cacheString;
			xmlDocPtr cacheXml;
			int retval;

			fseek(CACHEFD, 0L, SEEK_END);  /* Position to end of file */
			cacheSize = ftell(CACHEFD);     /* Get file length */
			rewind(CACHEFD);               /* Back to start of file */
			cacheString = (uint8_t *)malloc(cacheSize*sizeof(uint8_t)+1); /* string must be null terminated */
			fread(cacheString, 1, cacheSize, CACHEFD);
			cacheString[cacheSize] = '\0';
			cacheSize += 1;
			fclose(CACHEFD);
			cacheXml = xmlParseDoc(cacheString);
			free(cacheString);
			retval = lime_createMultipartMessage(cacheXml, (uint8_t *)msg, (uint8_t *)peer_uri, &multipartEncryptedMessage);
			if (retval != 0) {
				ms_warning("Unable to encrypt message for %s : %s - op [%p]", peer_uri, lime_error_code_to_string(retval), op);
				xmlFreeDoc(cacheXml);
				free(multipartEncryptedMessage);
				sal_error_info_set(&op->error_info, SalReasonNotAcceptable, 488, "Unable to encrypt IM", NULL);
				op->base.root->callbacks.text_delivery_update(op,SalTextDeliveryFailed);
				return 0;
			} else {
				/* dump updated cache to a string */
				xmlChar *xmlStringOutput;
				int xmlStringLength;
				xmlDocDumpFormatMemoryEnc(cacheXml, &xmlStringOutput, &xmlStringLength, "UTF-8", 0);
				/* write it to the cache file */
				CACHEFD = fopen(lc->zrtp_secrets_cache, "w+");
				fwrite(xmlStringOutput, 1, xmlStringLength, CACHEFD);
				xmlFree(xmlStringOutput);
				fclose(CACHEFD);
				content_length = strlen((const char *)multipartEncryptedMessage);
			}
			xmlFreeDoc(cacheXml);
		}
	}

	snprintf(content_type_raw,sizeof(content_type_raw),BELLE_SIP_CONTENT_TYPE ": %s",content_type);
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),BELLE_SIP_HEADER(belle_sip_header_content_type_parse(content_type_raw)));
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),BELLE_SIP_HEADER(belle_sip_header_content_length_create(content_length)));
	belle_sip_message_add_header(BELLE_SIP_MESSAGE(req),BELLE_SIP_HEADER(belle_sip_header_date_create_from_time(&curtime)));
	belle_sip_message_set_body(BELLE_SIP_MESSAGE(req),(multipartEncryptedMessage==NULL)?msg:(const char *)multipartEncryptedMessage,content_length);
	return sal_op_send_request(op,req);

}

int sal_message_reply(SalOp *op, SalReason reason){
	if (op->pending_server_trans){
		int code=sal_reason_to_sip_code(reason);
		belle_sip_response_t *resp = belle_sip_response_create_from_request(
			belle_sip_transaction_get_request((belle_sip_transaction_t*)op->pending_server_trans),code);
		belle_sip_server_transaction_send_response(op->pending_server_trans,resp);
		return 0;
	}else ms_error("sal_message_reply(): no server transaction");
	return -1;
}

int sal_text_send(SalOp *op, const char *from, const char *to, const char *msg) {
	return sal_message_send(op,from,to,"text/plain",msg, NULL);
}

static belle_sip_listener_callbacks_t op_message_callbacks={0};

void sal_op_message_fill_cbs(SalOp*op) {
	if (op_message_callbacks.process_io_error==NULL){
		op_message_callbacks.process_io_error=process_io_error;
		op_message_callbacks.process_response_event=process_response_event;
		op_message_callbacks.process_timeout=process_timeout;
		op_message_callbacks.process_request_event=process_request_event;
	}
	op->callbacks=&op_message_callbacks;
	op->type=SalOpMessage;
}
