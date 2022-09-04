/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2017, 2018, 2019, 2020, 2021 Giuseppe Scrivano <giuseppe@scrivano.org>
 * crun is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * crun is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */
#define _GNU_SOURCE

#include <config.h>
#include "../custom-handler.h"
#include "../container.h"
#include "../utils.h"
#include "../linux.h"
#include "handler-utils.h"
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sched.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef HAVE_DLOPEN
#  include <dlfcn.h>
#endif

#ifdef HAVE_WASM_NODEJS
#define NAPI_EMBEDDING
#include "js_native_api.h"
#endif

#if HAVE_DLOPEN && HAVE_WASM_NODEJS
typedef napi_status (*napi_create_platform_t)(int argc,
                                              char** argv,
                                              int exec_argc,
                                              char** exec_argv,
                                              char*** errors,
                                              int thread_pool_size,
                                              napi_platform* result);

typedef napi_status (*napi_create_environment_t)(napi_platform platform,
                                                 char*** errors,
                                                 const char* main_script,
                                                 napi_env* result);

typedef napi_status (*napi_run_script_t)(napi_env env,
                                         napi_value script,
                                         napi_value* result);

typedef napi_status (*napi_create_string_latin1_t)(napi_env env,
                                                   const char* str,
                                                   size_t length,
                                                   napi_value* result);

typedef napi_status (*napi_create_external_arraybuffer_t)(
		                 napi_env env,
                                 void* external_data,
                                 size_t byte_length,
                                 napi_finalize finalize_cb,
                                 void* finalize_hint,
                                 napi_value* result);

typedef napi_status (*napi_create_buffer_t)(napi_env env,
                                            size_t size,
                                            void** data,
                                            napi_value* result);

typedef napi_status (*napi_get_global_t)(napi_env env, napi_value* result);

typedef napi_status (*napi_set_property_t)(napi_env env,
                                           napi_value object,
                                           napi_value key,
                                           napi_value value);

typedef napi_status (*napi_create_typedarray_t)(napi_env env,
                                                napi_typedarray_type type,
                                                size_t length,
                                                napi_value arraybuffer,
                                                size_t byte_offset,
                                                napi_value* result);

typedef napi_status (*napi_create_external_buffer_t)(napi_env env,
                                                     size_t length,
                                                     void* data,
                                                     napi_finalize finalize_cb,
                                                     void* finalize_hint,
                                                     napi_value* result);

typedef napi_status (*napi_destroy_platform_t)(napi_platform platform,
                                               int *exit_code);

typedef napi_status (*napi_destroy_environment_t)(napi_env env);

typedef napi_status (*napi_run_environment_t)(napi_env env);

#define NODE_API_CHECK(message)                                                \
  do {                                                                         \
    if (status != napi_ok) {                                                   \
      error (EXIT_FAILURE, 0, message);                                        \
    }                                                                          \
  } while (0)

static int
libwasm_nodejs_exec (void *cookie, libcrun_container_t *container,
                const char *pathname, char *const argv[])
{
  /////////////////////////////////////////////////////////
  // load all of the required functions
  napi_create_platform_t create_platform = dlsym (cookie, "napi_create_platform");
  napi_create_environment_t create_environment = dlsym (cookie, "napi_create_environment");
  napi_create_string_latin1_t create_string_latin1 = dlsym (cookie, "napi_create_string_latin1");
  napi_run_script_t run_script = dlsym (cookie, "napi_run_script");
  napi_create_external_arraybuffer_t create_external_arraybuffer = dlsym (cookie, "napi_create_external_arraybuffer");
  napi_get_global_t get_global = dlsym (cookie, "napi_get_global");
  napi_set_property_t set_property = dlsym (cookie, "napi_set_property");
  napi_create_typedarray_t create_typedarray = dlsym (cookie, "napi_create_typedarray");
  napi_create_external_buffer_t create_external_buffer = dlsym (cookie, "napi_create_external_buffer");
  napi_run_environment_t run_environment = dlsym (cookie, "napi_run_environment");
  napi_destroy_platform_t destroy_platform = dlsym (cookie, "napi_destroy_platform");
  napi_destroy_environment_t destroy_environment = dlsym (cookie, "napi_destroy_environment");
  if ( create_platform == NULL ||
       create_environment == NULL ||
       create_string_latin1 == NULL ||
       create_external_arraybuffer == NULL ||
       get_global == NULL ||
       run_script == NULL ||
       create_typedarray == NULL ||
       create_external_buffer == NULL ||
       run_environment == NULL ||
       destroy_environment == NULL ||
       destroy_platform == NULL ||
       set_property == NULL) {
    error (EXIT_FAILURE, 0, "could not find symbol in `libnode.so`");
  }

  /////////////////////////////////////////////////////////
  // Load and parse container entrypoint
  FILE *file = fopen (pathname, "rb");
  if (! file)
    error (EXIT_FAILURE, 0, "error loading entrypoint");
  fseek (file, 0L, SEEK_END);
  size_t file_size = ftell (file);
  char* wasm_bytes = malloc(file_size);
  fseek (file, 0L, SEEK_SET);
  if (fread (wasm_bytes, file_size, 1, file) != 1)
    error (EXIT_FAILURE, 0, "error load");
  fclose (file);

  /////////////////////////////////////////////////////////g
  // run wasm with Node.js
  napi_status status;
  napi_platform platform;

  // wasi support is experimental, provide the required flag and
  // suppress warnings. This is of course a concern outside of
  // a PoC
  char* the_argv[] = {"libnode","--experimental-wasi-unstable-preview1", "--no-warnings"};
  status = create_platform(3, the_argv, 0, NULL, NULL, 0, &platform);
  NODE_API_CHECK("Failed to create platform");

  napi_env env;
  status = create_environment(platform, NULL, NULL , &env);
  NODE_API_CHECK("Failed to create environment");

  napi_value wasm_buffer = NULL;
  status = create_external_buffer(env, file_size, wasm_bytes, NULL, NULL, &wasm_buffer);
  NODE_API_CHECK("Failed to create buffer for wasm");

  napi_value global = NULL;
  status = get_global(env, &global);
  NODE_API_CHECK("Failed to get global object");

  napi_value wasm_key = NULL;
  status = create_string_latin1(env, "wasm", NAPI_AUTO_LENGTH, &wasm_key);
  NODE_API_CHECK("Failed to create wasm key string");
  status = set_property(env, global, wasm_key, wasm_buffer);
  NODE_API_CHECK("Failed to set wasm object on global object");

  napi_value script = NULL;
  char* script_chars = "(async () => { \
    const { WASI } = this.require('wasi'); \
    const wasi = new WASI({ preopens: { '.': '.' }}); \
    const wasm = await (this.WebAssembly.compile(this.wasm)); \
    const instance = await this.WebAssembly.instantiate(wasm, {wasi_snapshot_preview1: wasi.wasiImport}); \
    wasi.start(instance) \
  })();";
  status = create_string_latin1(env, script_chars, strlen(script_chars), &script);
  NODE_API_CHECK("Failed to create script to run wasm");

  napi_value result = NULL;
  status = run_script(env, script, &result);
  NODE_API_CHECK("failed to run script");

  status = run_environment(env);
  NODE_API_CHECK("failed to run environment");

  status = destroy_environment(env);
  NODE_API_CHECK("failed to destroy environment");

  int exit_code;
  status = destroy_platform(platform, &exit_code);
  NODE_API_CHECK("failed to destroy platform");
  exit (exit_code);
}

static int
libwasm_nodejs_load (void **cookie, libcrun_error_t *err arg_unused)
{
  void *handle;

  // HARD CODED PATH FOR NOW
  handle = dlopen ("libnode.so", RTLD_NOW);
  if (handle == NULL)
    return crun_make_error (err, 0, "could not load `libnode.so`: %s", dlerror ());
  *cookie = handle;

  return 0;
}

static int
libwasm_nodejs_unload (void *cookie, libcrun_error_t *err arg_unused)
{
  int r;

  if (cookie)
    {
      r = dlclose (cookie);
      if (UNLIKELY (r < 0))
        return crun_make_error (err, 0, "could not unload handle: %s", dlerror ());
    }
  return 0;
}

static int
libwasm_nodejs_can_handle_container (libcrun_container_t *container, libcrun_error_t *err arg_unused)
{
  return wasm_can_handle_container (container, err);
}

struct custom_handler_s handler_wasm_nodejs = {
  .name = "wasm_nodejs",
  .feature_string = "WASM:wasm_nodejs",
  .load = libwasm_nodejs_load,
  .unload = libwasm_nodejs_unload,
  .exec_func = libwasm_nodejs_exec,
  .can_handle_container = libwasm_nodejs_can_handle_container,
};

#endif
