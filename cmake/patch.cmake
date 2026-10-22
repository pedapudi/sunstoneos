# Apply one reviewed patch to produce a generated build source.
#
# Every hunk must match its recorded context exactly. GNU patch would otherwise
# accept up to two mismatched context lines (fuzz), and in a chain of patches a
# fuzzy hunk can land beside code it was never reviewed against. A hunk whose
# exact context sits a few lines away (offset) is still accepted.
function(apply_build_patch output source patch_file)
  execute_process(COMMAND patch --fuzz=0 --reject-file=- --output "${output}.tmp" "${source}" "${patch_file}"
    RESULT_VARIABLE status OUTPUT_VARIABLE report ERROR_VARIABLE report)
  if(NOT status EQUAL 0)
    file(REMOVE "${output}.tmp")
    message(FATAL_ERROR "Could not apply ${patch_file} exactly to ${source}:\n${report}")
  endif()
  # Preserve the generated source timestamp when patch output is unchanged.
  configure_file("${output}.tmp" "${output}" COPYONLY)
  file(REMOVE "${output}.tmp")
endfunction()
