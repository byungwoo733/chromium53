/*
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gbm_priv.h"
#include "helpers.h"

const struct gbm_driver gbm_driver_gma500 =
{
	.name = "gma500",
	.bo_create = gbm_dumb_bo_create,
	.bo_destroy = gbm_dumb_bo_destroy,
	.format_list = {
		{GBM_FORMAT_RGBX8888, GBM_BO_USE_SCANOUT | GBM_BO_USE_CURSOR | GBM_BO_USE_RENDERING},
	}
};
