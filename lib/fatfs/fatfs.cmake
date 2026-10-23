# Adds the pinned read-only FatFs volume and its SD port to one image target.
# fatfs_dir holds the files listed in source.json; each must match its hash.
function(sunstone_enable_fatfs target fatfs_dir)
  get_filename_component(fatfs_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../.." ABSOLUTE)
  set(fatfs_manifest_path "${fatfs_root}/lib/fatfs/source.json")
  file(READ "${fatfs_manifest_path}" fatfs_manifest)
  string(JSON fatfs_count LENGTH "${fatfs_manifest}" files)
  math(EXPR fatfs_last "${fatfs_count}-1")
  foreach(index RANGE 0 ${fatfs_last})
    string(JSON name MEMBER "${fatfs_manifest}" files ${index})
    string(JSON expected GET "${fatfs_manifest}" files "${name}")
    file(SHA256 "${fatfs_dir}/${name}" actual)
    if(NOT actual STREQUAL expected)
      message(FATAL_ERROR "Pinned FatFs input differs: ${fatfs_dir}/${name}")
    endif()
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${fatfs_manifest_path}")
  target_sources(${target} PRIVATE "${fatfs_dir}/ff.c" "${fatfs_dir}/ffunicode.c"
    "${fatfs_root}/drivers/storage/fatfs_port.c")
  # ff.h includes "ffconf.h" from its own directory first; force this one.
  set_source_files_properties("${fatfs_dir}/ff.c" "${fatfs_dir}/ffunicode.c" TARGET_DIRECTORY ${target}
    PROPERTIES COMPILE_OPTIONS "-include;${fatfs_root}/lib/fatfs/ffconf.h")
  target_include_directories(${target} PRIVATE "${fatfs_root}/lib/fatfs" "${fatfs_dir}")
endfunction()
