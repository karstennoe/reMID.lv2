cmake_minimum_required(VERSION 2.8.12)
project(reMID)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake-modules/")

find_package(PythonInterp)
if(PYTHONINTERP_FOUND)
  add_custom_target(generate_banks
      COMMAND ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/converter/build_banks.py
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Generating .swibank instrument banks"
  )
  # Ensure banks exist even if the user only runs `make install`.
  install(CODE "execute_process(COMMAND \"${PYTHON_EXECUTABLE}\" \"${CMAKE_CURRENT_SOURCE_DIR}/converter/build_banks.py\" WORKING_DIRECTORY \"${CMAKE_CURRENT_SOURCE_DIR}\")")
endif()

add_subdirectory(lib/) 
add_subdirectory(src/) 
install (DIRECTORY instruments/
    DESTINATION lib/lv2/remid.lv2
)

# Install the raw SID-Wizard instrument exports into the bundle.
install (DIRECTORY converter/sidwizard_instruments/
    DESTINATION lib/lv2/remid.lv2/instruments/swi
    FILES_MATCHING PATTERN "*.swi"
)
