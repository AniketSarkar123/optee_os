// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2014, STMicroelectronics International N.V.
 * Copyright (c) 2020, Arm Limited
 * Copyright (c) 2025, NVIDIA Corporation & AFFILIATES.
 */

#include <assert.h>
#include <kernel/mutex.h>
#include <kernel/panic.h>
#include <kernel/pseudo_ta.h>
#include <kernel/stmm_sp.h>
#include <kernel/tee_common.h>
#include <kernel/tee_misc.h>
#include <kernel/tee_ta_manager.h>
#include <kernel/tee_time.h>
#include <kernel/thread.h>
#include <kernel/user_mode_ctx.h>
#include <kernel/user_ta.h>
#include <malloc.h>
#include <mm/core_memprot.h>
#include <mm/core_mmu.h>
#include <mm/mobj.h>
#include <mm/vm.h>
#include <pta_stats.h>
#include <stdlib.h>
#include <string.h>
#include <tee_api_types.h>
#include <tee/entry_std.h>
#include <tee/tee_obj.h>
#include <trace.h>
#include <types_ext.h>
#include <user_ta_header.h>
#include <utee_types.h>
#include <util.h>

#if defined(CFG_TA_STATS)
#define MAX_DUMP_SESS_NUM	(16)

struct tee_ta_dump_ctx {
	TEE_UUID uuid;
	uint32_t panicked;
	bool is_user_ta;
	uint32_t sess_num;
	uint32_t sess_id[MAX_DUMP_SESS_NUM];
};
#endif

/* This mutex protects the critical section in tee_ta_init_session */
struct mutex tee_ta_mutex = MUTEX_INITIALIZER;
/* This condvar is used when waiting for a TA context to become initialized */
struct condvar tee_ta_init_cv = CONDVAR_INITIALIZER;
struct tee_ta_ctx_head tee_ctxes = TAILQ_HEAD_INITIALIZER(tee_ctxes);

#ifndef CFG_CONCURRENT_SINGLE_INSTANCE_TA
static struct condvar tee_ta_cv = CONDVAR_INITIALIZER;
static short int tee_ta_single_instance_thread = THREAD_ID_INVALID;
static size_t tee_ta_single_instance_count;
#endif

#ifdef CFG_CONCURRENT_SINGLE_INSTANCE_TA
static void lock_single_instance(void)
{
}

static void unlock_single_instance(void)
{
}

static bool has_single_instance_lock(void)
{
	return false;
}
#else
static void lock_single_instance(void)
{
	/* Requires tee_ta_mutex to be held */
	if (tee_ta_single_instance_thread != thread_get_id()) {
		/* Wait until the single-instance lock is available. */
		while (tee_ta_single_instance_thread != THREAD_ID_INVALID)
			condvar_wait(&tee_ta_cv, &tee_ta_mutex);

		tee_ta_single_instance_thread = thread_get_id();
		assert(tee_ta_single_instance_count == 0);
	}

	tee_ta_single_instance_count++;
}

static void unlock_single_instance(void)
{
	/* Requires tee_ta_mutex to be held */
	assert(tee_ta_single_instance_thread == thread_get_id());
	assert(tee_ta_single_instance_count > 0);

	tee_ta_single_instance_count--;
	if (tee_ta_single_instance_count == 0) {
		tee_ta_single_instance_thread = THREAD_ID_INVALID;
		condvar_signal(&tee_ta_cv);
	}
}

static bool has_single_instance_lock(void)
{
	/* Requires tee_ta_mutex to be held */
	return tee_ta_single_instance_thread == thread_get_id();
}
#endif

struct tee_ta_session *__noprof to_ta_session(struct ts_session *sess)
{
	assert(is_ta_ctx(sess->ctx) || is_stmm_ctx(sess->ctx));
	return container_of(sess, struct tee_ta_session, ts_sess);
}

static struct tee_ta_ctx *ts_to_ta_ctx(struct ts_ctx *ctx)
{
	if (is_ta_ctx(ctx))
		return to_ta_ctx(ctx);

	if (is_stmm_ctx(ctx))
		return &(to_stmm_ctx(ctx)->ta_ctx);

	panic("bad context");
}

static bool tee_ta_try_set_busy(struct tee_ta_ctx *ctx)
{
	bool rc = true;

	if (ctx->flags & TA_FLAG_CONCURRENT)
		return true;

	mutex_lock(&tee_ta_mutex);

	if (ctx->flags & TA_FLAG_SINGLE_INSTANCE)
		lock_single_instance();

	if (has_single_instance_lock()) {
		if (ctx->busy) {
			/*
			 * We're holding the single-instance lock and the
			 * TA is busy, as waiting now would only cause a
			 * dead-lock, we release the lock and return false.
			 */
			rc = false;
			if (ctx->flags & TA_FLAG_SINGLE_INSTANCE)
				unlock_single_instance();
		}
	} else {
		/*
		 * We're not holding the single-instance lock, we're free to
		 * wait for the TA to become available.
		 */
		while (ctx->busy)
			condvar_wait(&ctx->busy_cv, &tee_ta_mutex);
	}

	/* Either it's already true or we should set it to true */
	ctx->busy = true;

	mutex_unlock(&tee_ta_mutex);
	return rc;
}

static void tee_ta_set_busy(struct tee_ta_ctx *ctx)
{
	if (!tee_ta_try_set_busy(ctx))
		panic();
}

static void tee_ta_clear_busy(struct tee_ta_ctx *ctx)
{
	if (ctx->flags & TA_FLAG_CONCURRENT)
		return;

	mutex_lock(&tee_ta_mutex);

	assert(ctx->busy);
	ctx->busy = false;
	condvar_signal(&ctx->busy_cv);

	if (ctx->flags & TA_FLAG_SINGLE_INSTANCE)
		unlock_single_instance();

	mutex_unlock(&tee_ta_mutex);
}

static void dec_session_ref_count(struct tee_ta_session *s)
{
	assert(s->ref_count > 0);
	s->ref_count--;
	if (s->ref_count == 1)
		condvar_signal(&s->refc_cv);
}

void tee_ta_put_session(struct tee_ta_session *s)
{
	mutex_lock(&tee_ta_mutex);

	if (s->lock_thread == thread_get_id()) {
		s->lock_thread = THREAD_ID_INVALID;
		condvar_signal(&s->lock_cv);
	}
	dec_session_ref_count(s);

	mutex_unlock(&tee_ta_mutex);
}

static struct tee_ta_session *tee_ta_find_session_nolock(uint32_t id,
			struct tee_ta_session_head *open_sessions)
{
	struct tee_ta_session *s = NULL;
	struct tee_ta_session *found = NULL;

	TAILQ_FOREACH(s, open_sessions, link) {
		if (s->id == id) {
			found = s;
			break;
		}
	}

	return found;
}

struct tee_ta_session *tee_ta_find_session(uint32_t id,
			struct tee_ta_session_head *open_sessions)
{
	struct tee_ta_session *s = NULL;

	mutex_lock(&tee_ta_mutex);

	s = tee_ta_find_session_nolock(id, open_sessions);

	mutex_unlock(&tee_ta_mutex);

	return s;
}

struct tee_ta_session *tee_ta_get_session(uint32_t id, bool exclusive,
			struct tee_ta_session_head *open_sessions)
{
	struct tee_ta_session *s;

	mutex_lock(&tee_ta_mutex);

	while (true) {
		s = tee_ta_find_session_nolock(id, open_sessions);
		if (!s)
			break;
		if (s->unlink) {
			s = NULL;
			break;
		}
		s->ref_count++;
		if (!exclusive)
			break;

		assert(s->lock_thread != thread_get_id());

		while (s->lock_thread != THREAD_ID_INVALID && !s->unlink)
			condvar_wait(&s->lock_cv, &tee_ta_mutex);

		if (s->unlink) {
			dec_session_ref_count(s);
			s = NULL;
			break;
		}

		s->lock_thread = thread_get_id();
		break;
	}

	mutex_unlock(&tee_ta_mutex);
	return s;
}

static void tee_ta_unlink_session(struct tee_ta_session *s,
			struct tee_ta_session_head *open_sessions)
{
	mutex_lock(&tee_ta_mutex);

	assert(s->ref_count >= 1);
	assert(s->lock_thread == thread_get_id());
	assert(!s->unlink);

	s->unlink = true;
	condvar_broadcast(&s->lock_cv);

	while (s->ref_count != 1)
		condvar_wait(&s->refc_cv, &tee_ta_mutex);

	TAILQ_REMOVE(open_sessions, s, link);

	mutex_unlock(&tee_ta_mutex);
}

static void destroy_session(struct tee_ta_session *s,
			    struct tee_ta_session_head *open_sessions)
{
#if defined(CFG_FTRACE_SUPPORT)
	if (s->ts_sess.ctx && s->ts_sess.ctx->ops->dump_ftrace) {
		ts_push_current_session(&s->ts_sess);
		s->ts_sess.fbuf = NULL;
		s->ts_sess.ctx->ops->dump_ftrace(s->ts_sess.ctx);
		ts_pop_current_session();
	}
#endif

	tee_ta_unlink_session(s, open_sessions);
#if defined(CFG_TA_GPROF_SUPPORT)
	free(s->ts_sess.sbuf);
#endif
	free(s);
}

static void destroy_context(struct tee_ta_ctx *ctx)
{
	DMSG("Destroy TA ctx (0x%" PRIxVA ")",  (vaddr_t)ctx);

	condvar_destroy(&ctx->busy_cv);
	ctx->ts_ctx.ops->destroy(&ctx->ts_ctx);
}

/*
 * tee_ta_context_find - Find TA in session list based on a UUID (input)
 * Returns a pointer to the session
 */
static struct tee_ta_ctx *tee_ta_context_find(const TEE_UUID *uuid)
{
	struct tee_ta_ctx *ctx;

	TAILQ_FOREACH(ctx, &tee_ctxes, link) {
		if (memcmp(&ctx->ts_ctx.uuid, uuid, sizeof(TEE_UUID)) == 0)
			return ctx;
	}

	return NULL;
}

/* check if requester (client ID) matches session initial client */
static TEE_Result check_client(struct tee_ta_session *s, const TEE_Identity *id)
{
	if (id == KERN_IDENTITY)
		return TEE_SUCCESS;

	if (id == NSAPP_IDENTITY) {
		if (s->clnt_id.login == TEE_LOGIN_TRUSTED_APP) {
			DMSG("nsec tries to hijack TA session");
			return TEE_ERROR_ACCESS_DENIED;
		}
		return TEE_SUCCESS;
	}

	if (memcmp(&s->clnt_id, id, sizeof(TEE_Identity)) != 0) {
		DMSG("client id mismatch");
		return TEE_ERROR_ACCESS_DENIED;
	}
	return TEE_SUCCESS;
}

/*
 * Check if invocation parameters matches TA properties
 *
 * @s - current session handle
 * @param - already identified memory references hold a valid 'mobj'.
 *
 * Policy:
 * - All TAs can access 'non-secure' shared memory.
 * - All TAs can access TEE private memory (seccpy)
 * - Only SDP flagged TAs can accept SDP memory references.
 */
#ifndef CFG_SECURE_DATA_PATH
static bool check_params(struct tee_ta_session *sess __unused,
			 struct tee_ta_param *param __unused)
{
	/*
	 * When CFG_SECURE_DATA_PATH is not enabled, SDP memory references
	 * are rejected at OP-TEE core entry. Hence here all TAs have same
	 * permissions regarding memory reference parameters.
	 */
	return true;
}
#else
static bool check_params(struct tee_ta_session *sess,
			 struct tee_ta_param *param)
{
	int n;

	/*
	 * When CFG_SECURE_DATA_PATH is enabled, OP-TEE entry allows SHM and
	 * SDP memory references. Only TAs flagged SDP can access SDP memory.
	 */
	if (sess->ts_sess.ctx &&
	    ts_to_ta_ctx(sess->ts_sess.ctx)->flags & TA_FLAG_SECURE_DATA_PATH)
		return true;

	for (n = 0; n < TEE_NUM_PARAMS; n++) {
		uint32_t param_type = TEE_PARAM_TYPE_GET(param->types, n);
		struct param_mem *mem = &param->u[n].mem;

		if (param_type != TEE_PARAM_TYPE_MEMREF_INPUT &&
		    param_type != TEE_PARAM_TYPE_MEMREF_OUTPUT &&
		    param_type != TEE_PARAM_TYPE_MEMREF_INOUT)
			continue;
		if (!mem->size)
			continue;
		if (mobj_is_sdp_mem(mem->mobj))
			return false;
	}
	return true;
}
#endif

static void set_invoke_timeout(struct tee_ta_session *sess,
				      uint32_t cancel_req_to)
{
	TEE_Time current_time;
	TEE_Time cancel_time;

	if (cancel_req_to == TEE_TIMEOUT_INFINITE)
		goto infinite;

	if (tee_time_get_sys_time(&current_time) != TEE_SUCCESS)
		goto infinite;

	if (ADD_OVERFLOW(current_time.seconds, cancel_req_to / 1000,
			 &cancel_time.seconds))
		goto infinite;

	cancel_time.millis = current_time.millis + cancel_req_to % 1000;
	if (cancel_time.millis > 1000) {
		if (ADD_OVERFLOW(current_time.seconds, 1,
				 &cancel_time.seconds))
			goto infinite;

		cancel_time.seconds++;
		cancel_time.millis -= 1000;
	}

	sess->cancel_time = cancel_time;
	return;

infinite:
	sess->cancel_time.seconds = UINT32_MAX;
	sess->cancel_time.millis = UINT32_MAX;
}

/*-----------------------------------------------------------------------------
 * Close a Trusted Application and free available resources
 *---------------------------------------------------------------------------*/
TEE_Result tee_ta_close_session(struct tee_ta_session *csess,
				struct tee_ta_session_head *open_sessions,
				const TEE_Identity *clnt_id)
{
	struct tee_ta_session *sess = NULL;
	struct tee_ta_ctx *ctx = NULL;
	struct ts_ctx *ts_ctx = NULL;
	bool keep_crashed = false;
	bool keep_alive = false;

	DMSG("csess 0x%" PRIxVA " id %u",
	     (vaddr_t)csess, csess ? csess->id : UINT_MAX);

	if (!csess)
		return TEE_ERROR_ITEM_NOT_FOUND;

	sess = tee_ta_get_session(csess->id, true, open_sessions);

	if (!sess) {
		EMSG("session 0x%" PRIxVA " to be removed is not found",
		     (vaddr_t)csess);
		return TEE_ERROR_ITEM_NOT_FOUND;
	}

	if (check_client(sess, clnt_id) != TEE_SUCCESS) {
		tee_ta_put_session(sess);
		return TEE_ERROR_BAD_PARAMETERS; /* intentional generic error */
	}

	DMSG("Destroy session");

	ts_ctx = sess->ts_sess.ctx;
	if (!ts_ctx) {
		destroy_session(sess, open_sessions);
		return TEE_SUCCESS;
	}

	ctx = ts_to_ta_ctx(ts_ctx);
	if (ctx->panicked) {
		destroy_session(sess, open_sessions);
	} else {
		tee_ta_set_busy(ctx);
		set_invoke_timeout(sess, TEE_TIMEOUT_INFINITE);
		ts_ctx->ops->enter_close_session(&sess->ts_sess);
		destroy_session(sess, open_sessions);
		tee_ta_clear_busy(ctx);
	}

	mutex_lock(&tee_ta_mutex);

	if (ctx->ref_count <= 0)
		panic();

	ctx->ref_count--;
	if (ctx->flags & TA_FLAG_SINGLE_INSTANCE)
		keep_alive = ctx->flags & TA_FLAG_INSTANCE_KEEP_ALIVE;
	if (keep_alive)
		keep_crashed = ctx->flags & TA_FLAG_INSTANCE_KEEP_CRASHED;
	if (!ctx->ref_count &&
	    ((ctx->panicked && !keep_crashed) || !keep_alive)) {
		if (!ctx->is_releasing) {
			TAILQ_REMOVE(&tee_ctxes, ctx, link);
			ctx->is_releasing = true;
		}
		mutex_unlock(&tee_ta_mutex);

		destroy_context(ctx);
	} else
		mutex_unlock(&tee_ta_mutex);

	return TEE_SUCCESS;
}

static TEE_Result tee_ta_init_session_with_context(struct tee_ta_session *s,
						   const TEE_UUID *uuid)
{
	struct tee_ta_ctx *ctx = NULL;

	while (true) {
		ctx = tee_ta_context_find(uuid);
		if (!ctx)
			return TEE_ERROR_ITEM_NOT_FOUND;

		if (!ctx->is_initializing)
			break;
		/*
		 * Context is still initializing, wait here until it's
		 * fully initialized. Note that we're searching for the
		 * context again since it may have been removed while we
		 * where sleeping.
		 */
		condvar_wait(&tee_ta_init_cv, &tee_ta_mutex);
	}

	/*
	 * If the trusted service is not a single instance service (e.g. is
	 * a multi-instance TA) it should be loaded as a new instance instead
	 * of doing anything with this instance. So tell the caller that we
	 * didn't find the TA it the caller will load a new instance.
	 */
	if ((ctx->flags & TA_FLAG_SINGLE_INSTANCE) == 0)
		return TEE_ERROR_ITEM_NOT_FOUND;

	/*
	 * The trusted service is single instance, if it isn't multi session we
	 * can't create another session unless its reference is zero
	 */
	if (!(ctx->flags & TA_FLAG_MULTI_SESSION) && ctx->ref_count)
		return TEE_ERROR_BUSY;

	DMSG("Re-open trusted service %pUl", (void *)&ctx->ts_ctx.uuid);

	ctx->ref_count++;
	s->ts_sess.ctx = &ctx->ts_ctx;
	s->ts_sess.handle_scall = s->ts_sess.ctx->ops->handle_scall;
	return TEE_SUCCESS;
}

static uint32_t new_session_id(struct tee_ta_session_head *open_sessions)
{
	struct tee_ta_session *last = NULL;
	uint32_t saved = 0;
	uint32_t id = 1;

	last = TAILQ_LAST(open_sessions, tee_ta_session_head);
	if (last) {
		/* This value is less likely to be already used */
		id = last->id + 1;
		if (!id)
			id++; /* 0 is not valid */
	}

	saved = id;
	do {
		if (!tee_ta_find_session_nolock(id, open_sessions))
			return id;
		id++;
		if (!id)
			id++;
	} while (id != saved);

	return 0;
}

static TEE_Result tee_ta_init_session(TEE_ErrorOrigin *err,
				struct tee_ta_session_head *open_sessions,
				const TEE_UUID *uuid,
				struct tee_ta_session **sess)
{
	TEE_Result res;
	struct tee_ta_session *s = calloc(1, sizeof(struct tee_ta_session));

	*err = TEE_ORIGIN_TEE;
	if (!s)
		return TEE_ERROR_OUT_OF_MEMORY;

	s->cancel_mask = true;
	condvar_init(&s->refc_cv);
	condvar_init(&s->lock_cv);
	s->lock_thread = THREAD_ID_INVALID;
	s->ref_count = 1;

	mutex_lock(&tee_ta_mutex);
	s->id = new_session_id(open_sessions);
	if (!s->id) {
		res = TEE_ERROR_OVERFLOW;
		goto err_mutex_unlock;
	}

	TAILQ_INSERT_TAIL(open_sessions, s, link);

	/* Look for already loaded TA */
	res = tee_ta_init_session_with_context(s, uuid);
	if (res == TEE_SUCCESS || res != TEE_ERROR_ITEM_NOT_FOUND) {
		mutex_unlock(&tee_ta_mutex);
		goto out;
	}

	/* Look for secure partition */
	res = stmm_init_session(uuid, s);
	if (res == TEE_SUCCESS || res != TEE_ERROR_ITEM_NOT_FOUND) {
		mutex_unlock(&tee_ta_mutex);
		if (res == TEE_SUCCESS)
			res = stmm_complete_session(s);

		goto out;
	}

	/* Look for pseudo TA */
	res = tee_ta_init_pseudo_ta_session(uuid, s);
	if (res == TEE_SUCCESS || res != TEE_ERROR_ITEM_NOT_FOUND) {
		mutex_unlock(&tee_ta_mutex);
		goto out;
	}

	/* Look for user TA */
	res = tee_ta_init_user_ta_session(uuid, s);
	mutex_unlock(&tee_ta_mutex);
	if (res == TEE_SUCCESS)
		res = tee_ta_complete_user_ta_session(s);

out:
	if (!res) {
		*sess = s;
		return TEE_SUCCESS;
	}

	mutex_lock(&tee_ta_mutex);
	TAILQ_REMOVE(open_sessions, s, link);
err_mutex_unlock:
	mutex_unlock(&tee_ta_mutex);
	free(s);
	return res;
}

static void maybe_release_ta_ctx(struct tee_ta_ctx *ctx)
{
	bool was_releasing = false;
	bool keep_crashed = false;
	bool keep_alive = false;

	if (ctx->flags & TA_FLAG_SINGLE_INSTANCE)
		keep_alive = ctx->flags & TA_FLAG_INSTANCE_KEEP_ALIVE;
	if (keep_alive)
		keep_crashed = ctx->flags & TA_FLAG_INSTANCE_KEEP_CRASHED;

	/*
	 * Keep panicked TAs with SINGLE_INSTANCE, KEEP_ALIVE, and KEEP_CRASHED
	 * flags in the context list to maintain their panicked status and
	 * prevent respawning.
	 */
	if (!keep_crashed) {
		mutex_lock(&tee_ta_mutex);
		was_releasing = ctx->is_releasing;
		ctx->is_releasing = true;
		if (!was_releasing) {
			DMSG("Releasing panicked TA ctx");
			TAILQ_REMOVE(&tee_ctxes, ctx, link);
		}
		mutex_unlock(&tee_ta_mutex);

		if (!was_releasing)
			ctx->ts_ctx.ops->release_state(&ctx->ts_ctx);
	}
}

TEE_Result tee_ta_open_session(TEE_ErrorOrigin *err,
			       struct tee_ta_session **sess,
			       struct tee_ta_session_head *open_sessions,
			       const TEE_UUID *uuid,
			       const TEE_Identity *clnt_id,
			       uint32_t cancel_req_to,
			       struct tee_ta_param *param)
{
	TEE_Result res = TEE_SUCCESS;
	struct tee_ta_session *s = NULL;
	struct tee_ta_ctx *ctx = NULL;
	struct ts_ctx *ts_ctx = NULL;
	bool panicked = false;
	bool was_busy = false;

	res = tee_ta_init_session(err, open_sessions, uuid, &s);
	if (res != TEE_SUCCESS) {
		DMSG("init session failed 0x%x", res);
		return res;
	}

	if (!check_params(s, param))
		return TEE_ERROR_BAD_PARAMETERS;

	ts_ctx = s->ts_sess.ctx;
	ctx = ts_to_ta_ctx(ts_ctx);

	if (tee_ta_try_set_busy(ctx)) {
		if (!ctx->panicked) {
			/* Save identity of the owner of the session */
			s->clnt_id = *clnt_id;
			s->param = param;
			set_invoke_timeout(s, cancel_req_to);
			res = ts_ctx->ops->enter_open_session(&s->ts_sess);
			s->param = NULL;
		}

		panicked = ctx->panicked;
		if (panicked) {
			maybe_release_ta_ctx(ctx);
			res = TEE_ERROR_TARGET_DEAD;
		}

		tee_ta_clear_busy(ctx);
	} else {
		/* Deadlock avoided */
		res = TEE_ERROR_BUSY;
		was_busy = true;
	}

	/*
	 * Origin error equal to TEE_ORIGIN_TRUSTED_APP for "regular" error,
	 * apart from panicking.
	 */
	if (panicked || was_busy)
		*err = TEE_ORIGIN_TEE;
	else
		*err = s->err_origin;

	tee_ta_put_session(s);
	if (panicked || res != TEE_SUCCESS)
		tee_ta_close_session(s, open_sessions, KERN_IDENTITY);

	if (!res)
		*sess = s;
	else
		EMSG("Failed for TA %pUl. Return error %#"PRIx32, uuid, res);

	return res;
}

TEE_Result tee_ta_invoke_command(TEE_ErrorOrigin *err,
				 struct tee_ta_session *sess,
				 const TEE_Identity *clnt_id,
				 uint32_t cancel_req_to, uint32_t cmd,
				 struct tee_ta_param *param)
{
	struct tee_ta_ctx *ta_ctx = NULL;
	struct ts_ctx *ts_ctx = NULL;
	TEE_Result res = TEE_SUCCESS;
	bool panicked = false;

	if (check_client(sess, clnt_id) != TEE_SUCCESS)
		return TEE_ERROR_BAD_PARAMETERS; /* intentional generic error */

	if (!check_params(sess, param))
		return TEE_ERROR_BAD_PARAMETERS;

	ts_ctx = sess->ts_sess.ctx;
	ta_ctx = ts_to_ta_ctx(ts_ctx);

	tee_ta_set_busy(ta_ctx);

	if (!ta_ctx->panicked) {
		sess->param = param;
		set_invoke_timeout(sess, cancel_req_to);
		res = ts_ctx->ops->enter_invoke_cmd(&sess->ts_sess, cmd);
		sess->param = NULL;
	}

	panicked = ta_ctx->panicked;
	if (panicked) {
		maybe_release_ta_ctx(ta_ctx);
		res = TEE_ERROR_TARGET_DEAD;
	}

	tee_ta_clear_busy(ta_ctx);

	/*
	 * Origin error equal to TEE_ORIGIN_TRUSTED_APP for "regular" error,
	 * apart from panicking.
	 */
	if (panicked)
		*err = TEE_ORIGIN_TEE;
	else
		*err = sess->err_origin;

	/* Short buffer is not an effective error case */
	if (res != TEE_SUCCESS && res != TEE_ERROR_SHORT_BUFFER)
		DMSG("Error: %x of %d", res, *err);

	return res;
}

#if defined(CFG_TA_STATS)
static TEE_Result dump_ta_memstats(struct tee_ta_session *s,
				   struct tee_ta_param *param)
{
	TEE_Result res = TEE_SUCCESS;
	struct tee_ta_ctx *ctx = NULL;
	struct ts_ctx *ts_ctx = NULL;
	bool panicked = false;

	ts_ctx = s->ts_sess.ctx;
	if (!ts_ctx)
		return TEE_ERROR_ITEM_NOT_FOUND;

	ctx = ts_to_ta_ctx(ts_ctx);

	if (ctx->is_initializing)
		return TEE_ERROR_BAD_STATE;

	if (tee_ta_try_set_busy(ctx)) {
		if (!ctx->panicked) {
			s->param = param;
			set_invoke_timeout(s, TEE_TIMEOUT_INFINITE);
			res = ts_ctx->ops->dump_mem_stats(&s->ts_sess);
			s->param = NULL;
		}

		panicked = ctx->panicked;
		if (panicked) {
			maybe_release_ta_ctx(ctx);
			res = TEE_ERROR_TARGET_DEAD;
		}

		tee_ta_clear_busy(ctx);
	} else {
		/* Deadlock avoided */
		res = TEE_ERROR_BUSY;
	}

	return res;
}

static void init_dump_ctx(struct tee_ta_dump_ctx *dump_ctx)
{
	struct tee_ta_session *sess = NULL;
	struct tee_ta_session_head *open_sessions = NULL;
	struct tee_ta_ctx *ctx = NULL;
	unsigned int n = 0;

	nsec_sessions_list_head(&open_sessions);
	/*
	 * Scan all sessions opened from secure side by searching through
	 * all available TA instances and for each context, scan all opened
	 * sessions.
	 */
	TAILQ_FOREACH(ctx, &tee_ctxes, link) {
		unsigned int cnt = 0;

		if (!is_user_ta_ctx(&ctx->ts_ctx))
			continue;

		memcpy(&dump_ctx[n].uuid, &ctx->ts_ctx.uuid,
		       sizeof(ctx->ts_ctx.uuid));
		dump_ctx[n].panicked = ctx->panicked;
		dump_ctx[n].is_user_ta = is_user_ta_ctx(&ctx->ts_ctx);
		TAILQ_FOREACH(sess, open_sessions, link) {
			if (sess->ts_sess.ctx == &ctx->ts_ctx) {
				if (cnt == MAX_DUMP_SESS_NUM)
					break;

				dump_ctx[n].sess_id[cnt] = sess->id;
				cnt++;
			}
		}

		dump_ctx[n].sess_num = cnt;
		n++;
	}
}

static TEE_Result dump_ta_stats(struct tee_ta_dump_ctx *dump_ctx,
				struct pta_stats_ta *dump_stats,
				size_t ta_count)
{
	TEE_Result res = TEE_SUCCESS;
	struct tee_ta_session *sess = NULL;
	struct tee_ta_session_head *open_sessions = NULL;
	struct tee_ta_param param = { };
	unsigned int i = 0;
	unsigned int j = 0;

	nsec_sessions_list_head(&open_sessions);

	for (i = 0; i < ta_count; i++) {
		struct pta_stats_ta *stats = &dump_stats[i];

		memcpy(&stats->uuid, &dump_ctx[i].uuid,
		       sizeof(dump_ctx[i].uuid));
		stats->panicked = dump_ctx[i].panicked;
		stats->sess_num = dump_ctx[i].sess_num;

		/* Find a session from dump context */
		for (j = 0, sess = NULL; j < dump_ctx[i].sess_num && !sess; j++)
			sess = tee_ta_get_session(dump_ctx[i].sess_id[j], true,
						  open_sessions);

		if (!sess)
			continue;
		/* If session is existing, get its heap stats */
		memset(&param, 0, sizeof(struct tee_ta_param));
		param.types = TEE_PARAM_TYPES(TEE_PARAM_TYPE_VALUE_OUTPUT,
					      TEE_PARAM_TYPE_VALUE_OUTPUT,
					      TEE_PARAM_TYPE_VALUE_OUTPUT,
					      TEE_PARAM_TYPE_NONE);
		res = dump_ta_memstats(sess, &param);
		if (res == TEE_SUCCESS) {
			stats->heap.allocated = param.u[0].val.a;
			stats->heap.max_allocated = param.u[0].val.b;
			stats->heap.size = param.u[1].val.a;
			stats->heap.num_alloc_fail = param.u[1].val.b;
			stats->heap.biggest_alloc_fail = param.u[2].val.a;
			stats->heap.biggest_alloc_fail_used = param.u[2].val.b;
		} else {
			memset(&stats->heap, 0, sizeof(stats->heap));
		}
		tee_ta_put_session(sess);
	}

	return TEE_SUCCESS;
}

TEE_Result tee_ta_instance_stats(void *buf, size_t *buf_size)
{
	TEE_Result res = TEE_SUCCESS;
	struct pta_stats_ta *dump_stats = NULL;
	struct tee_ta_dump_ctx *dump_ctx = NULL;
	struct tee_ta_ctx *ctx = NULL;
	size_t sz = 0;
	size_t ta_count = 0;

	if (!buf_size)
		return TEE_ERROR_BAD_PARAMETERS;

	mutex_lock(&tee_ta_mutex);

	/* Go through all available TA and calc out the actual buffer size. */
	TAILQ_FOREACH(ctx, &tee_ctxes, link)
		if (is_user_ta_ctx(&ctx->ts_ctx))
			ta_count++;

	sz = sizeof(struct pta_stats_ta) * ta_count;
	if (!sz) {
		/* sz = 0 means there is no UTA, return no item found. */
		res = TEE_ERROR_ITEM_NOT_FOUND;
	} else if (!buf || *buf_size < sz) {
		/*
		 * buf is null or pass size less than actual size
		 * means caller try to query the buffer size.
		 * update *buf_size.
		 */
		*buf_size = sz;
		res = TEE_ERROR_SHORT_BUFFER;
	} else if (!IS_ALIGNED_WITH_TYPE(buf, uint32_t)) {
		DMSG("Data alignment");
		res = TEE_ERROR_BAD_PARAMETERS;
	} else {
		dump_stats = (struct pta_stats_ta *)buf;
		dump_ctx = malloc(sizeof(struct tee_ta_dump_ctx) * ta_count);
		if (!dump_ctx)
			res = TEE_ERROR_OUT_OF_MEMORY;
		else
			init_dump_ctx(dump_ctx);
	}
	mutex_unlock(&tee_ta_mutex);

	if (res != TEE_SUCCESS)
		return res;

	/* Dump user ta stats by iterating dump_ctx[] */
	res = dump_ta_stats(dump_ctx, dump_stats, ta_count);
	if (res == TEE_SUCCESS)
		*buf_size = sz;

	free(dump_ctx);
	return res;
}
#endif

TEE_Result tee_ta_cancel_command(TEE_ErrorOrigin *err,
				 struct tee_ta_session *sess,
				 const TEE_Identity *clnt_id)
{
	*err = TEE_ORIGIN_TEE;

	if (check_client(sess, clnt_id) != TEE_SUCCESS)
		return TEE_ERROR_BAD_PARAMETERS; /* intentional generic error */

	sess->cancel = true;
	return TEE_SUCCESS;
}

bool tee_ta_session_is_cancelled(struct tee_ta_session *s, TEE_Time *curr_time)
{
	TEE_Time current_time;

	if (s->cancel_mask)
		return false;

	if (s->cancel)
		return true;

	if (s->cancel_time.seconds == UINT32_MAX)
		return false;

	if (curr_time != NULL)
		current_time = *curr_time;
	else if (tee_time_get_sys_time(&current_time) != TEE_SUCCESS)
		return false;

	if (current_time.seconds > s->cancel_time.seconds ||
	    (current_time.seconds == s->cancel_time.seconds &&
	     current_time.millis >= s->cancel_time.millis)) {
		return true;
	}

	return false;
}

#if defined(CFG_TA_GPROF_SUPPORT)
void tee_ta_gprof_sample_pc(vaddr_t pc)
{
	struct ts_session *s = ts_get_current_session();
	struct user_ta_ctx *utc = NULL;
	struct sample_buf *sbuf = NULL;
	TEE_Result res = 0;
	size_t idx = 0;

	sbuf = s->sbuf;
	if (!sbuf || !sbuf->enabled)
		return; /* PC sampling is not enabled */

	idx = (((uint64_t)pc - sbuf->offset)/2 * sbuf->scale)/65536;
	if (idx < sbuf->nsamples) {
		utc = to_user_ta_ctx(s->ctx);
		res = vm_check_access_rights(&utc->uctx,
					     TEE_MEMORY_ACCESS_READ |
					     TEE_MEMORY_ACCESS_WRITE |
					     TEE_MEMORY_ACCESS_ANY_OWNER,
					     (uaddr_t)&sbuf->samples[idx],
					     sizeof(*sbuf->samples));
		if (res != TEE_SUCCESS)
			return;
		sbuf->samples[idx]++;
	}
	sbuf->count++;
}

static void gprof_update_session_utime(bool suspend, struct ts_session *s,
				       uint64_t now)
{
	struct sample_buf *sbuf = s->sbuf;

	if (!sbuf)
		return;

	if (suspend) {
		assert(sbuf->usr_entered);
		sbuf->usr += now - sbuf->usr_entered;
		sbuf->usr_entered = 0;
	} else {
		assert(!sbuf->usr_entered);
		if (!now)
			now++; /* 0 is reserved */
		sbuf->usr_entered = now;
	}
}

/*
 * Update user-mode CPU time for the current session
 * @suspend: true if session is being suspended (leaving user mode), false if
 * it is resumed (entering user mode)
 */
static void tee_ta_update_session_utime(bool suspend)
{
	struct ts_session *s = ts_get_current_session();
	uint64_t now = barrier_read_counter_timer();

	gprof_update_session_utime(suspend, s, now);
}

void tee_ta_update_session_utime_suspend(void)
{
	tee_ta_update_session_utime(true);
}

void tee_ta_update_session_utime_resume(void)
{
	tee_ta_update_session_utime(false);
}
#endif

#if defined(CFG_FTRACE_SUPPORT)
static void ftrace_update_times(bool suspend)
{
	struct ts_session *s = ts_get_current_session_may_fail();
	struct ftrace_buf *fbuf = NULL;
	uint64_t now = 0;
	uint32_t i = 0;

	if (!s)
		return;

	now = barrier_read_counter_timer();

	fbuf = s->fbuf;
	if (!fbuf)
		return;

	if (suspend) {
		fbuf->suspend_time = now;
	} else {
		for (i = 0; i <= fbuf->ret_idx; i++)
			fbuf->begin_time[i] += now - fbuf->suspend_time;
	}
}

void tee_ta_ftrace_update_times_suspend(void)
{
	ftrace_update_times(true);
}

void tee_ta_ftrace_update_times_resume(void)
{
	ftrace_update_times(false);
}
#endif

bool __noprof is_ta_ctx(struct ts_ctx *ctx)
{
	return is_user_ta_ctx(ctx) || is_pseudo_ta_ctx(ctx);
}
