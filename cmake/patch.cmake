# Apply one reviewed patch to produce a generated build source.
#
# Every hunk must match its recorded context exactly. GNU patch would otherwise
# accept up to two mismatched context lines (fuzz), and in a chain of patches a
# fuzzy hunk can land beside code it was never reviewed against. A hunk whose
# exact context sits a few lines away (offset) is still accepted.
function(apply_build_patch output source patch_file)
  # These patches were written against an earlier base and still need fuzz.
  # Each entry pins the output that was checked to place every hunk at its
  # intended location, so any further change to the base or the patch stops
  # configuration. Regenerate such a patch against its current base and remove
  # its entry.
  set(fuzzy_outputs
    "apps/crypto-single-core/main.patch|f2c1a63e4905880d80b6b014a60c534521d16565d243939e14651e736b9b1ccf"
    "apps/crypto-single-core/runtime.patch|7039c032a339afdba1037808c1135cf5897d78aa2dd2dc933bd5955c716e0477"
    "apps/pwm-single-core/main.patch|88488dfba09ae6974aec11d6f6a4ce2effe076dbe5d9b9b7e53e20d6d4083a33"
    "apps/pwm-single-core/runtime.patch|4d33c859731d36399dfc18c0ea253520c38b7e9665ecd78eafa7872a884f0cdc")
  execute_process(COMMAND patch --fuzz=0 --reject-file=- --output "${output}.tmp" "${source}" "${patch_file}"
    RESULT_VARIABLE status OUTPUT_VARIABLE report ERROR_VARIABLE report)
  if(NOT status EQUAL 0)
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    file(RELATIVE_PATH name "${root}" "${patch_file}")
    set(pinned "")
    foreach(entry IN LISTS fuzzy_outputs)
      string(REPLACE "|" ";" parts "${entry}")
      list(GET parts 0 listed)
      if(listed STREQUAL name)
        list(GET parts 1 pinned)
      endif()
    endforeach()
    set(digest "")
    if(NOT pinned STREQUAL "")
      execute_process(COMMAND patch --silent --reject-file=- --output "${output}.tmp" "${source}" "${patch_file}"
        RESULT_VARIABLE fuzzy_status)
      if(fuzzy_status EQUAL 0)
        file(SHA256 "${output}.tmp" digest)
      endif()
    endif()
    if(pinned STREQUAL "" OR NOT digest STREQUAL pinned)
      file(REMOVE "${output}.tmp")
      message(FATAL_ERROR "Could not apply ${patch_file} exactly to ${source}:\n${report}")
    endif()
  endif()
  # Preserve the generated source timestamp when patch output is unchanged.
  configure_file("${output}.tmp" "${output}" COPYONLY)
  file(REMOVE "${output}.tmp")
endfunction()
