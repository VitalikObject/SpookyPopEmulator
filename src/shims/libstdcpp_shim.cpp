#include "modules.h"
#include "shim_registry.h"

#include <array>

void RegisterLibStdCppShims(ShimRegistry& registry) {
    registry.AddResolver("__ZNSs4_Rep20_S_empty_rep_storageE", [](ShimContext& ctx) {
        return ctx.RegisterStdStringEmptyRep("__ZNSs4_Rep20_S_empty_rep_storageE");
    });
    registry.AddZeroData("__ZTVN10__cxxabiv117__class_type_infoE", 64);
    registry.AddZeroData("__ZTVN10__cxxabiv120__si_class_type_infoE", 64);
    registry.AddZeroData("__ZTVN10__cxxabiv121__vmi_class_type_infoE", 64);
    // typeinfo for std::runtime_error
    registry.AddZeroData("__ZTISt13runtime_error", 16);

    static constexpr std::array kGenericFunctions{
        "__ZdaPv",
        "__ZdlPv",
        "__Znam",
        "__Znwm",
        "___cxa_pure_virtual",
    };
    for (const char* symbol : kGenericFunctions) {
        registry.AddGenericFunction(symbol);
    }

    static constexpr std::array kCppFunctions{
        "___gxx_personality_sj0",
        "__ZNKSs6substrEmm",
        "__ZNKSs7compareEPKc",
        "__ZNKSs7compareERKSs",
        "__ZNSirsIjEERSiRT_",
        "__ZNSo5writeEPKci",
        "__ZNSs4_Rep10_M_destroyERKSaIcE",
        "__ZNSs6assignEPKcm",
        "__ZNSs6assignERKSs",
        "__ZNSs9push_backEc",
        "__ZNSsC1EPKcRKSaIcE",
        "__ZNSsC1ERKSs",
        "__ZNSsD1Ev",
        "__ZNSsD2Ev",
        "__ZNSt12logic_errorD1Ev",
        "__ZNSt13runtime_errorC1ERKSs",
        "__ZNSt13runtime_errorD1Ev",
        "__ZNSt13runtime_errorD2Ev",
        "__ZNSt14basic_ofstreamIcSt11char_traitsIcEE5closeEv",
        "__ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1EPKcSt13_Ios_Openmode",
        "__ZNSt14basic_ofstreamIcSt11char_traitsIcEED1Ev",
        "__ZNSt15_List_node_base4hookEPS_",
        "__ZNSt15_List_node_base6unhookEv",
        "__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEEC1ESt13_Ios_Openmode",
        "__ZNSt18basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev",
        "__ZNSt8ios_base4InitC1Ev",
        "__ZNSt8ios_base4InitD1Ev",
        "__ZNSt9basic_iosIcSt11char_traitsIcEE5clearESt12_Ios_Iostate",
        "__ZSt17__throw_bad_allocv",
        "__ZSt18_Rb_tree_decrementPSt18_Rb_tree_node_base",
        "__ZSt18_Rb_tree_incrementPKSt18_Rb_tree_node_base",
        "__ZSt18_Rb_tree_incrementPSt18_Rb_tree_node_base",
        "__ZSt20__throw_length_errorPKc",
        "__ZSt20__throw_out_of_rangePKc",
        "__ZSt28_Rb_tree_rebalance_for_erasePSt18_Rb_tree_node_baseRS_",
        "__ZSt29_Rb_tree_insert_and_rebalancebPSt18_Rb_tree_node_baseS0_RS_",
        "__ZSt9terminatev",
        "__ZStlsIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_PKS3_i",
        "___cxa_allocate_exception",
        "___cxa_begin_catch",
        "___cxa_call_unexpected",
        "___cxa_end_catch",
        "___cxa_free_exception",
        "___cxa_guard_abort",
        "___cxa_guard_acquire",
        "___cxa_guard_release",
        "___cxa_rethrow",
        "___cxa_throw",
    };
    for (const char* symbol : kCppFunctions) {
        registry.AddCppRuntimeFunction(symbol);
    }
}
