/*-
 * Copyright (c) 2013-2015 Varnish Software Group
 * All rights reserved.
 *
 * Author: Dag Haavi Finstad <daghf@varnish-software.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "vrt.h"
#include "cache/cache.h"
#include "cache/cache_director.h"
#include "cache/cache_backend.h"

#include "vcc_if.h"

struct trouble {
	unsigned		magic;
#define TROUBLE_MAGIC		0x4211ab21
	uint8_t			digest[DIGEST_LEN];
	double			timeout;
	VTAILQ_ENTRY(trouble)	list;
};

struct vmod_saintmode_saintmode {
	unsigned				magic;
#define VMOD_SAINTMODE_MAGIC			0xa03756e4
	struct director				sdir[1];
	const struct director			*be;
	pthread_mutex_t				mtx;
	unsigned				threshold;
	unsigned				n_trouble;
	VTAILQ_ENTRY(vmod_saintmode_saintmode)	list;
	VTAILQ_HEAD(, trouble)			troublelist;
};

struct saintmode_objs {
	unsigned				magic;
#define SAINTMODE_OBJS_MAGIC			0x9aa7beec
	VTAILQ_HEAD(, vmod_saintmode_saintmode) sm_list;
};

VCL_BACKEND __match_proto__(td_saintmode_saintmode_backend)
vmod_saintmode_backend(VRT_CTX, struct vmod_saintmode_saintmode *sm) {
	CHECK_OBJ_NOTNULL(sm, VMOD_SAINTMODE_MAGIC);
	CHECK_OBJ_NOTNULL(sm->sdir, DIRECTOR_MAGIC);
	return (sm->sdir);
}

static struct vmod_saintmode_saintmode *
find_sm(const struct saintmode_objs *sm_objs,
    const struct director *be) {
	struct vmod_saintmode_saintmode *sm;

	CHECK_OBJ_NOTNULL(sm_objs, SAINTMODE_OBJS_MAGIC);
	CHECK_OBJ_NOTNULL(be, DIRECTOR_MAGIC);

	VTAILQ_FOREACH(sm, &sm_objs->sm_list, list) {
		CHECK_OBJ_NOTNULL(sm, VMOD_SAINTMODE_MAGIC);
		CHECK_OBJ_NOTNULL(sm->be, DIRECTOR_MAGIC);
		if (sm->be == be)
			return (sm);
	}

	return (NULL);
}

VCL_VOID __match_proto__(td_saintmode_blacklist)
vmod_blacklist(VRT_CTX, struct vmod_priv *priv, VCL_DURATION expires) {
	struct trouble *tp;
	struct saintmode_objs *sm_objs;
	struct vbc *vbc;
	struct backend *be;
	struct vmod_saintmode_saintmode *sm;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (!ctx->bo || !ctx->bo->director_resp) {
		VSLb(ctx->vsl, SLT_VCL_Error, "saintmode.blacklist() called"
		    " outside of vcl_backend_response");
		return;
	}

	CHECK_OBJ_NOTNULL(ctx->bo, BUSYOBJ_MAGIC);
	CAST_OBJ_NOTNULL(sm_objs, priv->priv, SAINTMODE_OBJS_MAGIC);
	sm = find_sm(sm_objs, ctx->bo->director_resp);
	if (!sm) {
		VSLb(ctx->vsl, SLT_VCL_Error, "Error: saintmode.blacklist(): "
		    "Saintmode not configured for this backend.");
		return;
	}

	ALLOC_OBJ(tp, TROUBLE_MAGIC);
	AN(tp);
	memcpy(tp->digest, ctx->bo->digest, sizeof tp->digest);
	tp->timeout = ctx->bo->t_prev + expires;
	pthread_mutex_lock(&sm->mtx);
	VTAILQ_INSERT_HEAD(&sm->troublelist, tp, list);
	sm->n_trouble++;
	pthread_mutex_unlock(&sm->mtx);
}

/* All adapted from PHK's saintmode implementation in Varnish 3.0 */
unsigned __match_proto__(vdi_healthy_f)
healthy(const struct director *dir, const struct busyobj *bo, double *changed) {
	struct trouble *tr;
	struct trouble *tr2;
	unsigned retval;
	struct vmod_saintmode_saintmode *sm;
	VTAILQ_HEAD(, trouble)  troublelist;
	double now;

	CHECK_OBJ_NOTNULL(dir, DIRECTOR_MAGIC);
	CAST_OBJ_NOTNULL(sm, dir->priv, VMOD_SAINTMODE_MAGIC);
	CHECK_OBJ_NOTNULL(sm->be, DIRECTOR_MAGIC);

	/* If we don't have a bo with a digest to look at, we can't
	 * know if we are on the trouble list or not. Fall back to the
	 * backend's healthy() function. */
	if (!bo)
		return (sm->be->healthy(sm->be, bo, changed));

	/* Saintmode is disabled, or list is empty */
	if (sm->threshold == 0 || sm->n_trouble == 0)
		return (sm->be->healthy(sm->be, bo, changed));

	now = bo->t_prev;
	retval = 1;
	VTAILQ_INIT(&troublelist);
	pthread_mutex_lock(&sm->mtx);
	VTAILQ_FOREACH_SAFE(tr, &sm->troublelist, list, tr2) {
		CHECK_OBJ_NOTNULL(tr, TROUBLE_MAGIC);

		if (tr->timeout < now) {
			VTAILQ_REMOVE(&sm->troublelist, tr, list);
			VTAILQ_INSERT_HEAD(&troublelist, tr, list);
			sm->n_trouble--;
			continue;
		}

		if (!memcmp(tr->digest, bo->digest, sizeof tr->digest)) {
			retval = 0;
			break;
		}
	}
	if (sm->threshold <= sm->n_trouble)
		retval = 0;
	pthread_mutex_unlock(&sm->mtx);

	VTAILQ_FOREACH_SAFE(tr, &troublelist, list, tr2)
		FREE_OBJ(tr);

	return (retval ? sm->be->healthy(sm->be, bo, changed) : 0);
}

static const struct director *  __match_proto__(vdi_resolve_f)
resolve(const struct director *dir, struct worker *wrk, struct busyobj *bo) {
	struct vmod_saintmode_saintmode *sm;
	double changed = 0.0;

	CHECK_OBJ_NOTNULL(dir, DIRECTOR_MAGIC);
	CAST_OBJ_NOTNULL(sm, dir->priv, VMOD_SAINTMODE_MAGIC);

	if (!healthy(dir, bo, &changed))
		return (NULL);

	return (sm->be);
}

VCL_VOID  __match_proto__(td_saintmode_saintmode__init)
vmod_saintmode__init(VRT_CTX, struct vmod_saintmode_saintmode **smp,
    const char *vcl_name, struct vmod_priv *priv, VCL_BACKEND be,
    VCL_INT threshold) {
	struct vmod_saintmode_saintmode *sm;
	struct saintmode_objs *sm_objs;

	AN(smp);
	AZ(*smp);
	ALLOC_OBJ(sm, VMOD_SAINTMODE_MAGIC);
	AN(sm);
	*smp = sm;

	sm->threshold = threshold;
	sm->n_trouble = 0;
	AZ(pthread_mutex_init(&sm->mtx, NULL));
	CHECK_OBJ_NOTNULL(be, DIRECTOR_MAGIC);
	sm->be = be;
	VTAILQ_INIT(&sm->troublelist);

	sm->sdir->magic = DIRECTOR_MAGIC;
	sm->sdir->resolve = resolve;
	sm->sdir->healthy = healthy;
	REPLACE(sm->sdir->vcl_name, vcl_name);
	sm->sdir->name = "saintmode";
	sm->sdir->priv = sm;

	if (!priv->priv) {
		ALLOC_OBJ(sm_objs, SAINTMODE_OBJS_MAGIC);
		VTAILQ_INIT(&sm_objs->sm_list);
		priv->priv = sm_objs;
		priv->free = free;
	}

	CAST_OBJ_NOTNULL(sm_objs, priv->priv, SAINTMODE_OBJS_MAGIC);
	VTAILQ_INSERT_TAIL(&sm_objs->sm_list, sm, list);
}

VCL_VOID __match_proto__(td_saintmode_saintmode__fini)
vmod_saintmode__fini(struct vmod_saintmode_saintmode **smp) {
	struct trouble *tr, *tr2;
	struct vmod_saintmode_saintmode *sm;

	AN(smp);
	CHECK_OBJ_NOTNULL(*smp, VMOD_SAINTMODE_MAGIC);
	sm = *smp;
	*smp = NULL;

	VTAILQ_FOREACH_SAFE(tr, &sm->troublelist, list, tr2) {
		CHECK_OBJ_NOTNULL(tr, TROUBLE_MAGIC);
		VTAILQ_REMOVE(&sm->troublelist, tr, list);
		FREE_OBJ(tr);
	}

	free(sm->sdir->vcl_name);
	AZ(pthread_mutex_destroy(&sm->mtx));

	/* We can no longer refer to the sm_objs after this
	 * free. Should be fine, as this fini function will be called
	 * first when the VCL is getting unloaded. */
	FREE_OBJ(sm);
}
