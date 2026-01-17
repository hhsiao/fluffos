#include "base/package_api.h"

#include <unicode/translit.h>
#include <unicode/unistr.h>

#ifdef F_SET_TRANSCODING
void f_set_transcoding() {
  if (!command_giver || !command_giver->interactive) {
    if (st_num_arg) {
      pop_stack();
    }
    push_undefined();
    return;
  }

  auto *ip = command_giver->interactive;

  // Reset to no-transcoding
  if (!st_num_arg) {
    if (ip->in_translit) {
      delete reinterpret_cast<icu::Transliterator*>(ip->in_translit);
      ip->in_translit = nullptr;
    }
    if (ip->out_translit) {
      delete reinterpret_cast<icu::Transliterator*>(ip->out_translit);
      ip->out_translit = nullptr;
    }
    push_undefined();
    return;
  }

  // Set to specific transcoding
  const auto *translit_id = sp->u.string;
  
  UErrorCode error_code = U_ZERO_ERROR;
  icu::UnicodeString uTranslitId(translit_id, "UTF-8");
  
  // Create forward transliterator for output (e.g., Hant->Hans)
  icu::Transliterator *out_translit = icu::Transliterator::createInstance(
      uTranslitId, UTRANS_FORWARD, error_code);
  
  if (U_FAILURE(error_code)) {
    error("Fail to set transcoding to '%s', error: %s.", 
          translit_id, u_errorName(error_code));
  }
  
  // Create reverse transliterator for input (e.g., Hans->Hant)
  error_code = U_ZERO_ERROR;
  icu::Transliterator *in_translit = icu::Transliterator::createInstance(
      uTranslitId, UTRANS_REVERSE, error_code);
  
  if (U_FAILURE(error_code)) {
    delete out_translit;
    error("Fail to set reverse transcoding to '%s', error: %s.", 
          translit_id, u_errorName(error_code));
  }
  
  // Clean up old transliterators
  if (ip->in_translit) {
    delete reinterpret_cast<icu::Transliterator*>(ip->in_translit);
  }
  if (ip->out_translit) {
    delete reinterpret_cast<icu::Transliterator*>(ip->out_translit);
  }
  
  // Set new transliterators
  ip->in_translit = reinterpret_cast<void*>(in_translit);
  ip->out_translit = reinterpret_cast<void*>(out_translit);
  
  // Return the transliterator ID
  std::string result;
  out_translit->getID().toUTF8String(result);
  
  pop_stack();
  push_malloced_string(string_copy(result.c_str(), "f_set_transcoding"));
}
#endif

#ifdef F_QUERY_TRANSCODING
void f_query_transcoding() {
  if (!command_giver || !command_giver->interactive) {
    push_undefined();
    return;
  }

  auto *ip = command_giver->interactive;
  
  if (!ip || !ip->out_translit) {
    push_undefined();
    return;
  }
  
  // Cast void* to icu::Transliterator* to call methods
  auto *out_translit = reinterpret_cast<icu::Transliterator*>(ip->out_translit);
  
  std::string result;
  out_translit->getID().toUTF8String(result);
  
  push_malloced_string(string_copy(result.c_str(), "f_query_transcoding"));
}
#endif

#ifdef F_STRING_TRANSLIT
// Bonus: standalone string transliteration function
void f_string_translit() {
  const auto *translit_id = sp->u.string;
  const auto *data = (sp - 1)->u.string;
  auto len = SVALUE_STRLEN(sp - 1);
  
  UErrorCode error_code = U_ZERO_ERROR;
  icu::UnicodeString uTranslitId(translit_id, "UTF-8");
  
  icu::Transliterator *translit = icu::Transliterator::createInstance(
      uTranslitId, UTRANS_FORWARD, error_code);
  
  if (U_FAILURE(error_code)) {
    error("string_translit: Invalid transliterator '%s', error: %s.", 
          translit_id, u_errorName(error_code));
  }
  
  // Convert input to UnicodeString
  icu::UnicodeString ustr(data, static_cast<int32_t>(len), "UTF-8");
  
  // Perform transliteration
  translit->transliterate(ustr);
  
  // Convert back to UTF-8
  std::string result;
  ustr.toUTF8String(result);
  
  delete translit;
  
  pop_2_elems();
  push_malloced_string(string_copy(result.c_str(), "f_string_translit"));
}
#endif