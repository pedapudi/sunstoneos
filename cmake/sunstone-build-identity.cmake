include_guard(GLOBAL)

# Appends the SHA-256 of every file in the argument list to the identity string
# and records each one in the identity text as "<sha256>  <path>". The text
# lines before the first digest line are the identity's components, which
# the identity joins with ";". Hashing the identity gives the build id, so
# build-id-inputs.txt lets two differing build ids be compared input by input
# (checks/build-id-inputs.py rebuilds the identity from it).
function(sunstone_identity_digests identity_var text_var)
  set(identity "${${identity_var}}")
  set(text "${${text_var}}")
  foreach(input IN LISTS ARGN)
    file(SHA256 "${input}" digest)
    string(APPEND identity "${digest}")
    set(portable "${input}")
    if(COMMAND sunstone_portable_text)
      sunstone_portable_text(portable "${input}")
    endif()
    string(APPEND text "\n${digest}  ${portable}")
  endforeach()
  set(${identity_var} "${identity}" PARENT_SCOPE)
  set(${text_var} "${text}" PARENT_SCOPE)
endfunction()
