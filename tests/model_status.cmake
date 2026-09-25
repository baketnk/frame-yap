if(NOT DEFINED APP OR NOT DEFINED ROOT OR NOT DEFINED STAGE)
  message(FATAL_ERROR "APP, ROOT and STAGE required")
endif()
function(check app expected regex)
  execute_process(COMMAND "${app}" ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 15)
  if(NOT "${result}" STREQUAL "${expected}" OR NOT output MATCHES "${regex}")
    message(FATAL_ERROR "${ARGN}: exit=${result}, stdout=${output}, stderr=${error}")
  endif()
endfunction()
check("${APP}" 0 "\"schema\": 1.*\"models\"|\"models\".*\"schema\": 1" --list-models --json)
check("${APP}" 1 "\"state\": \"not_installed\"" --check-model redux --model-dir "${STAGE}/absent" --json)
check("${APP}" 2 "^$" --check-model "redux;echo" --model-dir "${STAGE}/absent" --json)
check("${APP}" 2 "^$" --check-model "../redux" --model-dir "${STAGE}/absent" --json)
# Minimal installed payload, independent of the source tree's script/module/asset paths.
file(MAKE_DIRECTORY "${STAGE}/bin" "${STAGE}/scripts" "${STAGE}/python/frameyap" "${STAGE}/assets/backends")
file(COPY "${APP}" DESTINATION "${STAGE}/bin")
file(COPY "${ROOT}/scripts/model-status.py" DESTINATION "${STAGE}/scripts")
file(GLOB modules "${ROOT}/python/frameyap/*.py")
file(COPY ${modules} DESTINATION "${STAGE}/python/frameyap")
file(COPY "${ROOT}/assets/backends/redux.json" DESTINATION "${STAGE}/assets/backends")
get_filename_component(name "${APP}" NAME)
check("${STAGE}/bin/${name}" 0 "redux.*unknown" --list-models)
check("${STAGE}/bin/${name}" 1 "\"reason\": \"directory_missing\"" --check-model redux --model-dir "${STAGE}/absent" --json)
# Tiny local pinned fixture exercises real hash verification through the CLI.
file(MAKE_DIRECTORY "${STAGE}/fixtures" "${STAGE}/models/toy")
file(WRITE "${STAGE}/models/toy/weights.bin" "hello")
file(SHA256 "${STAGE}/models/toy/weights.bin" pinned_hash)
file(WRITE "${STAGE}/fixtures/toy.json" "{\n"
  "\"schema\":1,\"id\":\"toy\",\"display_name\":\"Tiny fixture\",\n"
  "\"launcher\":{\"type\":\"python\",\"path\":\"python/frameyap/worker.py\","
  "\"arguments\":[\"{model_dir}\",\"{clip_dir}\"],\"protocol\":\"frameyap-worker-v1\"},\n"
  "\"model\":{\"source\":\"https://example.invalid/toy\",\"revision\":\"fixture\","
  "\"files\":[{\"path\":\"weights.bin\",\"size\":5,\"sha256\":\"${pinned_hash}\"}]},\n"
  "\"attribution\":\"fixture only\",\"license\":{\"id\":\"MIT\",\"text\":\"fixture only\"},"
  "\"requirements\":{\"cpu\":\"fixture only\",\"gpu\":\"none\"}}\n")
check("${STAGE}/bin/${name}" 0 "\"state\": \"installed_verified\""
  --check-model toy --manifest-dir "${STAGE}/fixtures" --model-dir "${STAGE}/models/toy" --json)
check("${STAGE}/bin/${name}" 0 "\"state\": \"installed_verified\""
  --list-models --manifest-dir "${STAGE}/fixtures" --model-dir "${STAGE}/models" --json)
file(WRITE "${STAGE}/models/toy/weights.bin" "HELLO")
check("${STAGE}/bin/${name}" 1 "\"reason\": \"hash_mismatch\""
  --check-model toy --manifest-dir "${STAGE}/fixtures" --model-dir "${STAGE}/models/toy" --json)
