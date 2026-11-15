include_guard(GLOBAL)

# Every image carries the revision of the source it was built from and whether tracked files had changes, as the object
# a733_source_revision (cmake/source-revision-write.cmake). The revision is read at every build, not at configure, so a commit between the
# two cannot leave a stale one, and it is not part of the build id (the id depends on the sources only). The runner reads the object
# and records it as the image's git_revision (tools/board.py, source_revisions); the HEAD of the host tree at run time is a separate
# field, because a worker can commit after building.
#
# Images are the executables that carry a build-id object. sunstone_embed_source_revisions runs once the whole tree is configured
# (cmake_language(DEFER) in CMakeLists.txt), finds them in every directory, and lists them in source-revision-targets.txt of the build
# directory.
function(sunstone_embed_source_revisions)
  set(output "${CMAKE_BINARY_DIR}/source-revision/source-revision.c")
  set(directories "${CMAKE_SOURCE_DIR}")
  set(images)
  while(directories)
    list(POP_FRONT directories directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
      get_target_property(type ${target} TYPE)
      if(NOT type STREQUAL "EXECUTABLE")
        continue()
      endif()
      get_target_property(sources ${target} SOURCES)
      foreach(source IN LISTS sources)
        if(source MATCHES "build-id\\.c$")
          list(APPEND images ${target})
          break()
        endif()
      endforeach()
    endforeach()
    get_property(subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    list(APPEND directories ${subdirectories})
  endwhile()
  if(NOT images)
    return()
  endif()
  # The target has no output that a commit changes, so it runs at every build and the recorded revision is HEAD when the build runs.
  # The file also exists after configure, for the generators that need their sources to exist.
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DOUTPUT=${output}"
    -P "${CMAKE_SOURCE_DIR}/cmake/source-revision-write.cmake")
  add_custom_target(sunstone-source-revision
    COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_SOURCE_DIR}" "-DOUTPUT=${output}"
      -P "${CMAKE_SOURCE_DIR}/cmake/source-revision-write.cmake"
    BYPRODUCTS "${output}" COMMENT "Recording the source revision" VERBATIM)
  set_source_files_properties("${output}" PROPERTIES GENERATED TRUE)
  foreach(image IN LISTS images)
    add_dependencies(${image} sunstone-source-revision)
    target_sources(${image} PRIVATE "${output}")
    # The linker drops sections that nothing references; the object is read from the ELF by name.
    target_link_options(${image} PRIVATE "LINKER:--undefined=a733_source_revision")
  endforeach()
  list(JOIN images "\n" lines)
  file(WRITE "${CMAKE_BINARY_DIR}/source-revision-targets.txt" "${lines}\n")
endfunction()
