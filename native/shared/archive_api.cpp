// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "host_api.hpp"
#include "archives.hpp"
#include <climits>
#include <cmath>
#include <cstring>

namespace {
bool path(JSContext* ctx, JSValueConst value, std::string& out) {
  if (!JS_IsString(value)) return false;
  std::size_t length = 0; const char* input = JS_ToCStringLen(ctx,&length,value);
  if (!input) return false;
  char resolved[PATH_MAX];
  const bool ok = length && length < PATH_MAX && !std::memchr(input,0,length) && host::resolve_path(input,resolved,sizeof resolved);
  if (ok) out = resolved;
  JS_FreeCString(ctx,input); return ok;
}
JSValue extract(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  if (argc < 3 || !JS_IsArray(argv[0])) return JS_ThrowTypeError(ctx,"archives.extract: expected sources, destination, maxBytes");
  archives::Request request;
  if (!path(ctx,argv[1],request.destination)) return JS_ThrowTypeError(ctx,"archives.extract: destination is not allowed");
  // Package resources are always read-only, even when console access is enabled.
  const char* destination = JS_ToCString(ctx,argv[1]);
  const bool readonly = destination && (!std::strcmp(destination,"/app0") || !std::strncmp(destination,"/app0/",6));
  if (destination) JS_FreeCString(ctx,destination);
  if (readonly) return JS_ThrowTypeError(ctx,"archives.extract: /app0 is read-only");
  double maximum = 0;
  if (JS_ToFloat64(ctx,&maximum,argv[2]) || !std::isfinite(maximum) || maximum < 1 || maximum > 1099511627776.0 || std::floor(maximum) != maximum)
    return JS_ThrowTypeError(ctx,"archives.extract: maxBytes must be an integer between 1 and 1 TiB");
  request.max_bytes = static_cast<std::uint64_t>(maximum);
  const JSValue length_value = JS_GetPropertyStr(ctx,argv[0],"length"); std::uint32_t length = 0;
  const int result = JS_ToUint32(ctx,&length,length_value); JS_FreeValue(ctx,length_value);
  if (result || !length || length > 1024) return JS_ThrowTypeError(ctx,"archives.extract: expected 1–1024 ordered volumes");
  for (std::uint32_t i = 0; i < length; ++i) {
    const JSValue value = JS_GetPropertyUint32(ctx,argv[0],i); std::string file;
    const bool ok = path(ctx,value,file); JS_FreeValue(ctx,value);
    if (!ok) return JS_ThrowTypeError(ctx,"archives.extract: source path is not allowed");
    request.sources.push_back(std::move(file));
  }
  std::string error; const auto id = archives::enqueue(std::move(request),error);
  if (!id) return JS_ThrowPlainError(ctx,"archives.extract: %s",error.c_str());
  return JS_NewUint32(ctx,id);
}
JSValue cancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
  std::uint32_t id = 0;
  if (!argc || JS_ToUint32(ctx,&id,argv[0])) return JS_ThrowTypeError(ctx,"archives.cancel: expected task id");
  archives::cancel(id); return JS_UNDEFINED;
}
JSValue poll(JSContext* ctx, JSValueConst, int, JSValueConst*) {
  JSValue out = JS_NewArray(ctx); std::uint32_t index = 0;
  for (const auto& snapshot : archives::poll()) {
    JSValue item = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,item,"id",JS_NewUint32(ctx,snapshot.id));
    JS_SetPropertyStr(ctx,item,"state",JS_NewString(ctx,snapshot.state.c_str()));
    JS_SetPropertyStr(ctx,item,"error",JS_NewString(ctx,snapshot.error.c_str()));
    JS_SetPropertyStr(ctx,item,"written",JS_NewFloat64(ctx,snapshot.written));
    JS_SetPropertyStr(ctx,item,"entries",JS_NewFloat64(ctx,snapshot.entries));
    JSValue artifacts = JS_NewArray(ctx); std::uint32_t at = 0;
    for (const auto& path : snapshot.artifacts) JS_SetPropertyUint32(ctx,artifacts,at++,JS_NewString(ctx,path.c_str()));
    JS_SetPropertyStr(ctx,item,"artifacts",artifacts);
    JS_SetPropertyUint32(ctx,out,index++,item);
  }
  return out;
}
}
JSValue ps5_react_archive_api(JSContext* ctx) {
  JSValue api = JS_NewObject(ctx);
  JS_SetPropertyStr(ctx,api,"extract",JS_NewCFunction(ctx,extract,"extract",3));
  JS_SetPropertyStr(ctx,api,"cancel",JS_NewCFunction(ctx,cancel,"cancel",1));
  JS_SetPropertyStr(ctx,api,"poll",JS_NewCFunction(ctx,poll,"poll",0));
  return api;
}
