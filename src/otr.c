/*
 * Off-the-Record Messaging (OTR) modules for IRC
 *
 * Copyright (C) 2008  Uli Meis <a.sporto+bee@gmail.com>
 *               2012  David Goulet <dgoulet@ev0ke.net>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301,USA
 */

#define _GNU_SOURCE
#include <assert.h>
#include <gcrypt.h>
#include <unistd.h>

#include "key.h"

static const char *statusbar_txt[] = {
	"FINISHED",
	"TRUST_MANUAL",
	"TRUST_SMP",
	"SMP_ABORT",
	"SMP_STARTED",
	"SMP_RESPONDED",
	"SMP_INCOMING",
	"SMP_FINALIZE",
	"SMP_ABORTED",
	"PEER_FINISHED",
	"SMP_FAILED",
	"SMP_SUCCESS",
	"GONE_SECURE",
	"GONE_INSECURE",
	"CTX_UPDATE"
};

#ifdef HAVE_GREGEX_H
GRegex *regex_policies;
#endif

static char *create_account_name(SERVER_REC *irssi)
{
	char *accname = NULL;

	assert(irssi);

	/* Valid or NULL, the caller should handle this */
	(void) asprintf(&accname, "%s@%s", IRSSI_NICK(irssi),
			IRSSI_CONN_ADDR(irssi));

	return accname;
}

/*
 * Load instance tags.
 */
static void instag_load(struct otr_user_state *ustate)
{
	int ret;
	char *filename;
	gcry_error_t err;

	assert(ustate);

	/* Getting the otr instance filename path */
	ret = asprintf(&filename, "%s%s", get_client_config_dir(),
			OTR_INSTAG_FILE);
	if (ret < 0) {
		goto error_filename;
	}

	ret = access(filename, F_OK);
	if (ret < 0) {
		IRSSI_DEBUG("%9OTR%9: no instance tags found at %9%s%9", filename);
		goto end;
	}

	err = otrl_instag_read(ustate->otr_state, filename);
	if (err == GPG_ERR_NO_ERROR) {
		IRSSI_DEBUG("%9OTR%9: Instance tags loaded from %9%s%9", filename);
	} else {
		IRSSI_DEBUG("%9OTR%9: Error loading instance tags: %d (%d)",
				gcry_strerror(err), gcry_strsource(err));
	}

end:
	free(filename);
error_filename:
	return;
}

static void destroy_peer_context_cb(void *data)
{
	struct otr_peer_context *opc = data;

	if (opc) {
		free(opc);
	}

	IRSSI_DEBUG("%9OTR%9: Peer context freed");
}

static void add_peer_context_cb(void *data, ConnContext *context)
{
	struct otr_peer_context *opc;

	opc = otr_create_peer_context();
	if (!opc) {
		return;
	}

	context->active_fingerprint = context->active_fingerprint;

	context->app_data = opc;
	context->app_data_free = destroy_peer_context_cb;

	IRSSI_MSG("%9OTR%9: Peer context created for %s", context->username);
}

/*
 * Get a context from a pair.
 */
ConnContext *otr_find_context(SERVER_REC *irssi, const char *nick, int create)
{
	char *accname = NULL;
	ConnContext *ctx = NULL;

	assert(irssi);
	assert(nick);

	accname = create_account_name(irssi);
	if (!accname) {
		goto error;
	}

	ctx = otrl_context_find(user_state_global->otr_state, nick, accname,
			OTR_PROTOCOL_ID, OTRL_INSTAG_BEST, create, NULL,
			add_peer_context_cb, irssi);

	free(accname);

error:
	return ctx;
}

struct otr_peer_context *otr_create_peer_context(void)
{
	struct otr_peer_context *opc;

	return zmalloc(sizeof(*opc));
}

/*
 * Return a newly allocated OTR user state for the given username.
 */
struct otr_user_state *otr_init_user(const char *user)
{
	struct otr_user_state *ous = NULL;

	assert(user);

	ous = zmalloc(sizeof(*ous));
	if (!ous) {
		goto error;
	}

	ous->otr_state = otrl_userstate_create();

	instag_load(ous);

	/* Load keys and fingerprints. */
	key_load(ous);
	key_load_fingerprints(ous);

error:
	return ous;
}

void otr_free_user(struct otr_user_state *ustate)
{
	key_generation_abort(ustate, TRUE);

	if (ustate->otr_state) {
		otrl_userstate_free(ustate->otr_state);
		ustate->otr_state = NULL;
	}

	otr_setpolicies(ustate, "", FALSE);
	otr_setpolicies(ustate, "", TRUE);

	free(ustate);
}

/*
 * init otr lib.
 */
void otr_lib_init()
{
	OTRL_INIT;

#ifdef HAVE_GREGEX_H
	regex_policies = g_regex_new(
			"([^,]+) (never|manual|handlews|opportunistic|always)(,|$)",
			0, 0, NULL);
#endif
}

/*
 * deinit otr lib.
 */
void otr_lib_uninit()
{
#ifdef HAVE_GREGEX_H
	g_regex_unref(regex_policies);
#endif
}

/*
 * Hand the given message to OTR.
 *
 * Return 0 if the message was successfully handled or else a negative value.
 */
int otr_send(SERVER_REC *irssi, const char *msg, const char *to, char **otr_msg)
{
	gcry_error_t err;
	char *accname = NULL;
	ConnContext *ctx = NULL;

	assert(irssi);

	accname = create_account_name(irssi);
	if (!accname) {
		goto error;
	}

	IRSSI_DEBUG("%9OTR%9: Sending message...");

	err = otrl_message_sending(user_state_global->otr_state, &otr_ops,
		irssi, accname, OTR_PROTOCOL_ID, to, OTRL_INSTAG_BEST, msg, NULL, otr_msg,
		OTRL_FRAGMENT_SEND_ALL_BUT_LAST, &ctx, add_peer_context_cb, irssi);
	if (err) {
		IRSSI_NOTICE(irssi, to, "%9OTR:%9 Send failed.");
		goto error;
	}

	IRSSI_DEBUG("%9OTR%9: Message sent...");

	/* Add peer context to OTR context if none exists. */
	if (ctx && !ctx->app_data) {
		add_peer_context_cb(irssi, ctx);
	}

	free(accname);
	return 0;

error:
	free(accname);
	return -1;
}

struct ctxlist_ *otr_contexts(struct otr_user_state *ustate)
{
	ConnContext *context;
	Fingerprint *fprint;
	struct ctxlist_ *ctxlist = NULL, *ctxhead = NULL;
	struct fplist_ *fplist, *fphead;
	char fp[OTRL_PRIVKEY_FPRINT_HUMAN_LEN];
	char *trust;

	for (context = ustate->otr_state->context_root; context;
			context = context->next) {
		if (!ctxlist) {
			ctxhead = ctxlist = g_malloc0(sizeof(struct ctxlist_));
		} else {
			ctxlist = ctxlist->next = g_malloc0(sizeof(struct ctxlist_));
		}

		switch (context->msgstate) {
		case OTRL_MSGSTATE_PLAINTEXT:
			ctxlist->state = STUNENCRYPTED;
			break;
		case OTRL_MSGSTATE_ENCRYPTED:
			ctxlist->state = STENCRYPTED;
			break;
		case OTRL_MSGSTATE_FINISHED:
			ctxlist->state = STFINISHED;
			break;
		default:
			ctxlist->state = STUNKNOWN;
			break;
		}

		ctxlist->username = context->username;
		ctxlist->accountname = context->accountname;

		fplist = fphead = NULL;
		for (fprint = context->fingerprint_root.next; fprint;
				fprint = fprint->next) {
			if (!fplist) {
				fphead = fplist = g_malloc0(sizeof(struct fplist_));
			} else {
				fplist = fplist->next = g_malloc0(sizeof(struct fplist_));
			}

			trust = fprint->trust ? : "";

			otrl_privkey_hash_to_human(fp, fprint->fingerprint);

			fplist->fp = g_strdup(fp);
			if (*trust == '\0') {
				fplist->authby = NOAUTH;
			} else if (strncmp(trust, "smp", strlen("smp")) == 0) {
				fplist->authby = AUTHSMP;
			} else {
				fplist->authby = AUTHMAN;
			}
		}

		ctxlist->fplist = fphead;
	}

	return ctxhead;
}

/*
 * Finish the conversation.
 */
void otr_finish(SERVER_REC *irssi, char *nick, const char *peername, int inquery)
{
	ConnContext *co;
	char nickbuf[128];

	if (peername) {
		nick = nickbuf;
		irssi = find_irssi_ctx_by_peername(peername, nick);
		if (!irssi) {
			goto end;
		}
	}

	if (!(co = otr_find_context(irssi, nick, FALSE))) {
		goto end;
	}

	otrl_message_disconnect(user_state_global->otr_state, &otr_ops, irssi,
			co->accountname, OTR_PROTOCOL_ID, nick, co->their_instance);

	otr_status_change(irssi, nick, OTR_STATUS_FINISHED);

	if (inquery) {
		otr_info(irssi, nick, TXT_CMD_FINISH, nick, IRSSI_CONN_ADDR(irssi));
	} else {
		otr_infost(TXT_CMD_FINISH, nick, IRSSI_CONN_ADDR(irssi));
	}

end:
	return;
}

void otr_finishall(struct otr_user_state *ustate)
{
	ConnContext *context;
	int finished = 0;
	SERVER_REC *irssi;

	for (context = ustate->otr_state->context_root; context;
			context = context->next) {
		if (context->msgstate != OTRL_MSGSTATE_ENCRYPTED) {
			continue;
		}

		irssi = find_irssi_ctx_by_peername(context->accountname,
				context->username);
		if (!irssi) {
			IRSSI_DEBUG("%9OTR%9: Unable to find context on /otr finishall "
					"for username %s", context->username);
			continue;
		}

		otrl_message_disconnect(ustate->otr_state, &otr_ops, irssi,
					context->accountname, OTR_PROTOCOL_ID, context->username,
					context->their_instance);
		otr_status_change(irssi, context->username, OTR_STATUS_FINISHED);

		otr_infost(TXT_CMD_FINISH, context->username, IRSSI_CONN_ADDR(irssi));
		finished++;
	}

	if (!finished) {
		otr_infost(TXT_CMD_FINISHALL_NONE);
	}
}

/*
 * Trust our peer.
 */
void otr_trust(SERVER_REC *irssi, char *nick, const char *peername)
{
	ConnContext *ctx;
	char nickbuf[128];
	char peerfp[OTRL_PRIVKEY_FPRINT_HUMAN_LEN];
	struct otr_peer_context *opc;

	if (peername) {
		nick = nickbuf;
		irssi = find_irssi_ctx_by_peername(peername, nick);
		if (!irssi) {
			goto end;
		}
	}

	ctx = otr_find_context(irssi, nick, FALSE);
	if (!ctx) {
		goto end;
	}

	opc = ctx->app_data;
	assert(opc);

	otrl_context_set_trust(opc->active_fingerprint, "manual");
	otr_status_change(irssi, nick, OTR_STATUS_TRUST_MANUAL);

	otrl_privkey_hash_to_human(peerfp, opc->active_fingerprint->fingerprint);

	IRSSI_NOTICE(irssi, nick, "%9OTR%9: Trusting fingerprint from %9%s%9:\n"
			"%9OTR%9: %g%s%n", nick, peerfp);

	key_write_fingerprints(user_state_global);

end:
	return;
}

/*
 * Abort any ongoing SMP authentication.
 */
void otr_abort_auth(ConnContext *co, SERVER_REC *irssi, const char *nick)
{
	otr_notice(irssi, nick,
			co->smstate->nextExpected != OTRL_SMP_EXPECT1 ?
			TXT_AUTH_ABORTED_ONGOING : TXT_AUTH_ABORTED);

	otrl_message_abort_smp(user_state_global->otr_state, &otr_ops,
			irssi, co);
	otr_status_change(irssi, nick, OTR_STATUS_SMP_ABORT);
}

/*
 * implements /otr authabort
 */
void otr_authabort(SERVER_REC *irssi, char *nick, const char *peername)
{
	ConnContext *co;
	char nickbuf[128];

	if (peername) {
		nick = nickbuf;
		irssi = find_irssi_ctx_by_peername(peername, nick);
		if (!irssi) {
			goto end;
		}
	}

	if (!(co = otr_find_context(irssi, nick, FALSE))) {
		otr_noticest(TXT_CTX_NOT_FOUND, co->accountname, nick);
		goto end;
	}

	otr_abort_auth(co, irssi, nick);

end:
	return;
}

/*
 * Initiate or respond to SMP authentication.
 */
void otr_auth(SERVER_REC *irssi, char *nick, const char *peername,
		const char *question, const char *secret)
{
	int ret;
	ConnContext *ctx;
	char nickbuf[128];
	struct otr_peer_context *opc;

	if (peername) {
		nick = nickbuf;
		irssi = find_irssi_ctx_by_peername(peername, nick);
		if (!irssi) {
			goto end;
		}
	}

	ctx = otr_find_context(irssi, nick, 0);
	if (!ctx) {
		otr_noticest(TXT_CTX_NOT_FOUND, ctx->accountname, nick);
		goto end;
	}

	opc = ctx->app_data;
	/* Again, code flow error. */
	assert(opc);

	if (ctx->msgstate != OTRL_MSGSTATE_ENCRYPTED) {
		IRSSI_NOTICE(irssi, nick,
				"%9OTR%9: You need to establish an OTR session before you "
				"can authenticate.");
		goto end;
	}

	/* Aborting an ongoing auth */
	if (ctx->smstate->nextExpected != OTRL_SMP_EXPECT1) {
		otr_abort_auth(ctx, irssi, nick);
	}

	/* reset trust level */
	if (ctx->active_fingerprint) {
		ret = otrl_context_is_fingerprint_trusted(ctx->active_fingerprint);
		if (!ret) {
			otrl_context_set_trust(ctx->active_fingerprint, "");
			key_write_fingerprints(user_state_global);
		}
	}

	if (opc->ask_secret) {
		otrl_message_respond_smp(user_state_global->otr_state, &otr_ops,
				irssi, ctx, (unsigned char *) secret, strlen(secret));
		otr_status_change(irssi, nick, OTR_STATUS_SMP_RESPONDED);
		IRSSI_NOTICE(irssi, nick, "%9OTR%9: Responding to authentication...");
	} else {
		if (question) {
			otrl_message_initiate_smp_q(user_state_global->otr_state,
				&otr_ops, irssi, ctx, question, (unsigned char *) secret,
				strlen(secret));
		} else {
			otrl_message_initiate_smp(user_state_global->otr_state,
				&otr_ops, irssi, ctx, (unsigned char *) secret,
				strlen(secret));
		}
		otr_status_change(irssi, nick, OTR_STATUS_SMP_STARTED);
		IRSSI_NOTICE(irssi, nick, "%9OTR%9: Initiated authentication...");
	}

	opc->ask_secret = 0;

end:
	return;
}

/*
 * Hand the given message to OTR.
 *
 * Returns 0 if its an OTR protocol message or else negative value.
 */
int otr_receive(SERVER_REC *irssi, const char *msg, const char *from,
		char **new_msg)
{
	int ret = -1;
	char *accname = NULL;
	OtrlTLV *tlvs;
	ConnContext *ctx;

	accname = create_account_name(irssi);
	if (!accname) {
		goto error;
	}

	IRSSI_DEBUG("%9OTR%9: Receiving message...");

	ret = otrl_message_receiving(user_state_global->otr_state,
		&otr_ops, irssi, accname, OTR_PROTOCOL_ID, from, msg, new_msg, &tlvs,
		&ctx, add_peer_context_cb, irssi);
	if (ret) {
		IRSSI_DEBUG("%9OTR%9: Ignoring message of length %d from %s to %s.\n"
				"%9OTR%9: %s", strlen(msg), from, accname, msg);
	} else {
		if (*new_msg) {
			IRSSI_DEBUG("%9OTR%9: Converted received message.");
		}
	}

	/* Add peer context to OTR context if non exists */
	if (ctx && !ctx->app_data) {
		add_peer_context_cb(irssi, ctx);
	}

	/* Check for disconnected message */
	OtrlTLV *tlv = otrl_tlv_find(tlvs, OTRL_TLV_DISCONNECTED);
	if (tlv) {
		otr_status_change(irssi, from, OTR_STATUS_PEER_FINISHED);
		IRSSI_NOTICE(irssi, from, "%9OTR%9: %9%s%9 has finished the OTR "
				"conversation. If you want to continue talking enter "
				"%9/otr finish%9 for plaintext or %9/otr init%9 to restart.",
				from);
	}

	otrl_tlv_free(tlvs);

	IRSSI_DEBUG("%9OTR%9: Message received.");

error:
	free(accname);
	return ret;
}

void otr_setpolicies(struct otr_user_state *ustate, const char *policies, int known)
{
#ifdef HAVE_GREGEX_H
	GMatchInfo *match_info;
	GSList *plist = known ? ustate->policy_known_list : ustate->policy_unknown_list;

	if (plist) {
		GSList *p = plist;
		do {
			struct plistentry *ple = p->data;
			g_pattern_spec_free(ple->namepat);
			g_free(p->data);
		} while ((p = g_slist_next(p)));

		g_slist_free(plist);
		plist = NULL;
	}

	g_regex_match(regex_policies, policies, 0, &match_info);

	while (g_match_info_matches(match_info)) {
		struct plistentry *ple =
			(struct plistentry *) g_malloc0(sizeof(struct plistentry));
		char *pol = g_match_info_fetch(match_info, 2);

		ple->namepat = g_pattern_spec_new(g_match_info_fetch(match_info, 1));

		switch (*pol) {
		case 'n':
			ple->policy = OTRL_POLICY_NEVER;
			break;
		case 'm':
			ple->policy = OTRL_POLICY_MANUAL;
			break;
		case 'h':
			ple->policy = OTRL_POLICY_MANUAL | OTRL_POLICY_WHITESPACE_START_AKE;
			break;
		case 'o':
			ple->policy = OTRL_POLICY_OPPORTUNISTIC;
			break;
		case 'a':
			ple->policy = OTRL_POLICY_ALWAYS;
			break;
		}

		plist = g_slist_append(plist, ple);

		g_free(pol);

		g_match_info_next(match_info, NULL);
	}

	g_match_info_free(match_info);

	if (known)
		ustate->policy_known_list = plist;
	else
		ustate->policy_unknown_list = plist;
#endif
}

/*
 * Get the OTR status of this conversation.
 */
int otr_getstatus(SERVER_REC *irssi, const char *nick)
{
	int ret, code = 0;
	ConnContext *ctx = NULL;

	assert(irssi);

	ctx = otr_find_context(irssi, nick, FALSE);
	if (!ctx) {
		code = IO_ST_PLAINTEXT;
		goto end;
	}

	switch (ctx->msgstate) {
	case OTRL_MSGSTATE_PLAINTEXT:
		code = IO_ST_PLAINTEXT;
		break;
	case OTRL_MSGSTATE_ENCRYPTED:
		/* Begin by checking trust. */
		ret = otrl_context_is_fingerprint_trusted(ctx->active_fingerprint);
		if (ret) {
			code = IO_ST_TRUST_SMP;
		} else {
			code = IO_ST_UNTRUSTED;
		}
		break;
	case OTRL_MSGSTATE_FINISHED:
		code = IO_ST_FINISHED;
		break;
	default:
		otr_logst(MSGLEVEL_CRAP,
				"BUG Found! Please write us a mail and describe how you got here");
		code = IO_ST_UNKNOWN;
		break;
	}

end:
	if (ctx) {
		IRSSI_DEBUG("Code: %d, state: %d, sm_prog_state: %d, auth state: %d",
				code, ctx->msgstate, ctx->smstate->sm_prog_state,
				ctx->auth.authstate);
	}
	return code;
}


/*
 * Get a format describing the OTR status of this conversation.
 */
int otr_getstatus_format(SERVER_REC *irssi, const char *nick)
{
	int status = otr_getstatus(irssi, nick);

	switch (status) {
	case IO_ST_PLAINTEXT:
		return TXT_ST_PLAINTEXT;
	case IO_ST_FINISHED:
		return TXT_ST_FINISHED;
	case IO_ST_UNTRUSTED:
		return TXT_ST_UNTRUSTED;
	case IO_ST_SMP_INCOMING:
		return TXT_ST_SMP_INCOMING;
	case IO_ST_SMP_OUTGOING:
		return TXT_ST_SMP_OUTGOING;
	case IO_ST_SMP_FINALIZE:
		return TXT_ST_SMP_FINALIZE;
	case IO_ST_TRUST_MANUAL:
		return TXT_ST_TRUST_MANUAL;
	case IO_ST_TRUST_SMP:
		return TXT_ST_TRUST_SMP;
	default:
		return TXT_ST_SMP_UNKNOWN;
	}
}

/*
 * Change status bar text for a given nickname.
 */
void otr_status_change(SERVER_REC *irssi, const char *nick,
		enum otr_status_event event)
{
	statusbar_items_redraw("otr");
	signal_emit("otr event", 3, irssi, nick, statusbar_txt[event]);
}

/*
 * Search for a OTR Fingerprint object from the given human readable string and
 * return a pointer to the object if found else NULL.
 */
Fingerprint *otr_find_hash_fingerprint_from_human(const char *human_fp,
		struct otr_user_state *ustate)
{
	char str_fp[OTRL_PRIVKEY_FPRINT_HUMAN_LEN];
	Fingerprint *fp = NULL, *fp_iter = NULL;
	ConnContext *context;

	/* Loop on all context of the user state */
	for (context = ustate->otr_state->context_root; context != NULL;
			context = context->next) {
		/* Loop on all fingerprint of the context */
		for (fp_iter = context->fingerprint_root.next; fp_iter;
				fp_iter = fp_iter->next) {
			otrl_privkey_hash_to_human(str_fp, fp_iter->fingerprint);
			/* Compare human fingerprint given in argument to the current. */
			if (strncmp(str_fp, human_fp, sizeof(str_fp)) == 0) {
				fp = otrl_context_find_fingerprint(context,
						fp_iter->fingerprint, 0, NULL);
				goto end;
			}
		}
	}

end:
	return fp;
}

/*
 * Forget a fingerprint.
 *
 * If str_fp is not NULL, it must be on the OTR human format like this:
 * "487FFADA 5073FEDD C5AB5C14 5BB6C1FF 6D40D48A". If str_fp is NULL, get the
 * context of the target nickname, check for the OTR peer context active
 * fingerprint and forget this one if possible.
 */
void otr_forget(SERVER_REC *irssi, const char *nick, char *str_fp,
		struct otr_user_state *ustate)
{
	char fp[OTRL_PRIVKEY_FPRINT_HUMAN_LEN];
	Fingerprint *fp_forget;
	ConnContext *ctx;
	struct otr_peer_context *opc;

	if (!irssi && !str_fp) {
		IRSSI_NOTICE(NULL, nick, "%9OTR%9: Need a fingerprint!");
		goto error;
	}

	/* No human string fingerprint given. */
	if (!str_fp) {
		ctx = otr_find_context(irssi, nick, FALSE);
		if (!ctx) {
			goto error;
		}

		opc = ctx->app_data;
		/* Always NEED a peer context or else code error. */
		assert(opc);

		fp_forget = opc->active_fingerprint;
	} else {
		fp_forget = otr_find_hash_fingerprint_from_human(str_fp, ustate);
	}

	IRSSI_DEBUG("%9OTR%9: Forgetting fingerprint: %s",
			(str_fp != NULL) ? str_fp : fp);

	if (fp_forget) {
		/* Don't do anything if context is in encrypted state. */
		if (fp_forget->context->msgstate == OTRL_MSGSTATE_ENCRYPTED) {
			IRSSI_NOTICE(irssi, nick, "%9OTR%9: Fingerprint "
					"context is still encrypted. Finish the OTR "
					"session beforehands.");
			goto end;
		}

		otrl_privkey_hash_to_human(fp, fp_forget->fingerprint);
		/* Forget fp and context if it's the only one remaining. */
		otrl_context_forget_fingerprint(fp_forget, 1);
		/* Update fingerprints file. */
		key_write_fingerprints(ustate);
		IRSSI_NOTICE(irssi, nick, "%9OTR%9: Fingerprint %y%s%n forgotten.",
				fp);
	} else {
		IRSSI_NOTICE(irssi, nick, "%9OTR%9: Fingerprint %y%s%n NOT found",
				(str_fp != NULL) ? str_fp : fp);
	}

end:
error:
	return;
}

/*
 * Distrust a fingerprint.
 *
 * If str_fp is not NULL, it must be on the OTR human format like this:
 * "487FFADA 5073FEDD C5AB5C14 5BB6C1FF 6D40D48A". If str_fp is NULL, get the
 * context of the target nickname, check for the OTR peer context active
 * fingerprint and distrust it.
 */
void otr_distrust(SERVER_REC *irssi, const char *nick, char *str_fp,
		struct otr_user_state *ustate)
{
	int ret;
	char fp[OTRL_PRIVKEY_FPRINT_HUMAN_LEN];
	Fingerprint *fp_distrust;
	ConnContext *ctx;
	struct otr_peer_context *opc;

	if (!irssi && !str_fp) {
		IRSSI_NOTICE(NULL, nick, "%9OTR%9: Need a fingerprint!");
		goto error;
	}

	/* No human string fingerprint given. */
	if (!str_fp) {
		ctx = otr_find_context(irssi, nick, FALSE);
		if (!ctx) {
			goto error;
		}

		opc = ctx->app_data;
		/* Always NEED a peer context or else code error. */
		assert(opc);

		fp_distrust = opc->active_fingerprint;
	} else {
		fp_distrust = otr_find_hash_fingerprint_from_human(str_fp, ustate);
	}

	IRSSI_DEBUG("%9OTR%9: Distrust fingerprint: %s",
			(str_fp != NULL) ? str_fp : fp);

	if (fp_distrust) {
		ret = otrl_context_is_fingerprint_trusted(fp_distrust);
		if (!ret) {
			/* Fingerprint already not trusted. Do nothing. */
			goto end;
		}

		otrl_privkey_hash_to_human(fp, fp_distrust->fingerprint);
		otrl_context_set_trust(fp_distrust, "");
		/* Update fingerprints file. */
		key_write_fingerprints(ustate);
		IRSSI_NOTICE(irssi, nick, "%9OTR%9: Fingerprint %y%s%n distrusted.",
				fp);
	} else {
		IRSSI_NOTICE(irssi, nick, "%9OTR%9: Fingerprint %y%s%n NOT found",
				(str_fp != NULL) ? str_fp : fp);
	}

end:
error:
	return;
}
