#pragma once

#include <eosio/chain/wasm_interface.hpp>
//#include <eosio/chain/webassembly/wavm.hpp>
#include <eosio/chain/webassembly/binaryen.hpp>
#include <eosio/chain/webassembly/runtime_interface.hpp>
#include <eosio/chain/wasm_eosio_injection.hpp>
#include <eosio/chain/transaction_context.hpp>
#include <fc/scoped_exit.hpp>

#include "IR/Module.h"
#include "Runtime/Intrinsics.h"
#include "Platform/Platform.h"
#include "WAST/WAST.h"
#include "IR/Validate.h"

#include <dlfcn.h>


using namespace fc;
using namespace eosio::chain::webassembly;
using namespace IR;
using namespace Runtime;

#if defined(__APPLE__) && defined(__MACH__)
   #define NATIVE_PLATFORM 1
#elif defined(__linux__)
   #define NATIVE_PLATFORM 2
#elif defined(_WIN64)
   #define NATIVE_PLATFORM 3
#else
   #error Not Supported Platform
#endif

void resume_billing_timer();
void pause_billing_timer();
const char* get_code( uint64_t receiver, size_t* size );

int db_api_find_i64( uint64_t code, uint64_t scope, uint64_t table, uint64_t id );
int32_t db_api_get_i64_ex( int iterator, uint64_t* primary, char* buffer, size_t buffer_size );
const char* db_api_get_i64_exex( int itr, size_t* buffer_size );


namespace eosio { namespace chain {
   void register_vm_api(void* handle);
   typedef void (*fn_apply)(uint64_t receiver, uint64_t account, uint64_t act);

   struct native_code_cache {
         uint32_t version;
         void *handle;
         fn_apply apply;
   };

   struct wasm_interface_impl {
      wasm_interface_impl(wasm_interface::vm_type vm) {
         if(vm == wasm_interface::vm_type::wavm) {
//            runtime_interface = std::make_unique<webassembly::wavm::wavm_runtime>();
         }
         else if(vm == wasm_interface::vm_type::binaryen)
            runtime_interface = std::make_unique<webassembly::binaryen::binaryen_runtime>();
         else
            FC_THROW("wasm_interface_impl fall through");
         //init_native_contract();
      }

      std::vector<uint8_t> parse_initial_memory(const Module& module) {
         std::vector<uint8_t> mem_image;

         for(const DataSegment& data_segment : module.dataSegments) {
            FC_ASSERT(data_segment.baseOffset.type == InitializerExpression::Type::i32_const);
            FC_ASSERT(module.memories.defs.size());
            const U32 base_offset = data_segment.baseOffset.i32;
            const Uptr memory_size = (module.memories.defs[0].type.size.min << IR::numBytesPerPageLog2);
            if(base_offset >= memory_size || base_offset + data_segment.data.size() > memory_size)
               FC_THROW_EXCEPTION(wasm_execution_error, "WASM data segment outside of valid memory range");
            if(base_offset + data_segment.data.size() > mem_image.size())
               mem_image.resize(base_offset + data_segment.data.size(), 0x00);
            memcpy(mem_image.data() + base_offset, data_segment.data.data(), data_segment.data.size());
         }

         return mem_image;
      }

      std::unique_ptr<wasm_instantiated_module_interface>& get_instantiated_module( const uint64_t& code_id )
      {
         size_t size = 0;
         const char* code = get_code( code_id, &size );

         auto it = instantiation_cache.find(code_id);
         if(it == instantiation_cache.end()) {
            auto timer_pause = fc::make_scoped_exit([&](){
               resume_billing_timer();
            });
            pause_billing_timer();
            IR::Module module;
            try {
               Serialization::MemoryInputStream stream((const U8*)code, size);
               WASM::serialize(stream, module);
               module.userSections.clear();
            } catch(const Serialization::FatalSerializationException& e) {
               EOS_ASSERT(false, wasm_serialization_error, e.message.c_str());
            } catch(const IR::ValidationException& e) {
               EOS_ASSERT(false, wasm_serialization_error, e.message.c_str());
            }

            wasm_injections::wasm_binary_injection injector(module);
            injector.inject();

            std::vector<U8> bytes;
            try {
               Serialization::ArrayOutputStream outstream;
               WASM::serialize(outstream, module);
               bytes = outstream.getBytes();
            } catch(const Serialization::FatalSerializationException& e) {
               EOS_ASSERT(false, wasm_serialization_error, e.message.c_str());
            } catch(const IR::ValidationException& e) {
               EOS_ASSERT(false, wasm_serialization_error, e.message.c_str());
            }
            it = instantiation_cache.emplace(code_id, runtime_interface->instantiate_module((const char*)bytes.data(), bytes.size(), parse_initial_memory(module))).first;
         }
         return it->second;
      }

      void init_native_contract() {
         uint64_t native_account[] = {N(eosio.bios), N(eosio.msig), N(eosio.token), N(eosio)/*eosio.system*/, N(exchange)};
         for (int i=0; i<sizeof(native_account)/sizeof(native_account[0]); i++) {
            load_native_contract(native_account[i]);
         }
      }

      fn_apply load_native_contract(uint64_t _account) {
         string contract_path;
         uint64_t native = N(native);
         void *handle = nullptr;
         char _name[64];
         snprintf(_name, sizeof(_name), "%s.%d", name(_account).to_string().c_str(), NATIVE_PLATFORM);
         uint64_t __account = NN(_name);

         int itr = db_api_find_i64(native, native, native, __account);
         if (itr < 0) {
            return nullptr;
         }

         size_t native_size = 0;
         const char* code = db_api_get_i64_exex(itr, &native_size);
         uint32_t version = *(uint32_t*)code;

         char native_path[64];
         sprintf(native_path, "%s.%d",name(__account).to_string().c_str(), version);

         wlog("loading native contract:\t ${n}", ("n", native_path));

         struct stat _s;
         if (stat(native_path, &_s) == 0) {
            //
         } else {
            std::ofstream out(native_path, std::ios::binary | std::ios::out);
            out.write(&code[4], native_size - 4);
            out.close();
         }
         contract_path = native_path;

         handle = dlopen(contract_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
         if (!handle) {
            return nullptr;
         }
         register_vm_api(handle);
         fn_apply _apply = (fn_apply)dlsym(handle, "apply");

         std::unique_ptr<native_code_cache> _cache = std::make_unique<native_code_cache>();
         _cache->version = version;
         _cache->handle = handle;
         _cache->apply = _apply;
   //      native_cache.emplace(_account, std::move(_cache));
         native_cache[_account] =  std::move(_cache);
         return _apply;
      }

      std::unique_ptr<wasm_runtime_interface> runtime_interface;
      map<uint64_t, std::unique_ptr<native_code_cache>> native_cache;
      map<uint64_t, std::unique_ptr<wasm_instantiated_module_interface>> instantiation_cache;
   };

//#define _REGISTER_INTRINSIC_EXPLICIT(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)\
//   _REGISTER_WAVM_INTRINSIC(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)
//   _REGISTER_BINARYEN_INTRINSIC(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)

#define _REGISTER_INTRINSIC_EXPLICIT(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)\
   _REGISTER_BINARYEN_INTRINSIC(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)

#define _REGISTER_INTRINSIC4(CLS, MOD, METHOD, WASM_SIG, NAME, SIG)\
   _REGISTER_INTRINSIC_EXPLICIT(CLS, MOD, METHOD, WASM_SIG, NAME, SIG )

#define _REGISTER_INTRINSIC3(CLS, MOD, METHOD, WASM_SIG, NAME)\
   _REGISTER_INTRINSIC_EXPLICIT(CLS, MOD, METHOD, WASM_SIG, NAME, decltype(&CLS::METHOD) )

#define _REGISTER_INTRINSIC2(CLS, MOD, METHOD, WASM_SIG)\
   _REGISTER_INTRINSIC_EXPLICIT(CLS, MOD, METHOD, WASM_SIG, BOOST_PP_STRINGIZE(METHOD), decltype(&CLS::METHOD) )

#define _REGISTER_INTRINSIC1(CLS, MOD, METHOD)\
   static_assert(false, "Cannot register " BOOST_PP_STRINGIZE(CLS) ":" BOOST_PP_STRINGIZE(METHOD) " without a signature");

#define _REGISTER_INTRINSIC0(CLS, MOD, METHOD)\
   static_assert(false, "Cannot register " BOOST_PP_STRINGIZE(CLS) ":<unknown> without a method name and signature");

#define _UNWRAP_SEQ(...) __VA_ARGS__

#define _EXPAND_ARGS(CLS, MOD, INFO)\
   ( CLS, MOD, _UNWRAP_SEQ INFO )

#define _REGISTER_INTRINSIC(R, CLS, INFO)\
   BOOST_PP_CAT(BOOST_PP_OVERLOAD(_REGISTER_INTRINSIC, _UNWRAP_SEQ INFO) _EXPAND_ARGS(CLS, "env", INFO), BOOST_PP_EMPTY())

#define REGISTER_INTRINSICS(CLS, MEMBERS)\
   BOOST_PP_SEQ_FOR_EACH(_REGISTER_INTRINSIC, CLS, _WRAPPED_SEQ(MEMBERS))

#define _REGISTER_INJECTED_INTRINSIC(R, CLS, INFO)\
   BOOST_PP_CAT(BOOST_PP_OVERLOAD(_REGISTER_INTRINSIC, _UNWRAP_SEQ INFO) _EXPAND_ARGS(CLS, EOSIO_INJECTED_MODULE_NAME, INFO), BOOST_PP_EMPTY())

#define REGISTER_INJECTED_INTRINSICS(CLS, MEMBERS)\
   BOOST_PP_SEQ_FOR_EACH(_REGISTER_INJECTED_INTRINSIC, CLS, _WRAPPED_SEQ(MEMBERS))

} } // eosio::chain