cmake_minimum_required(VERSION 3.16)

# Pull in libosdp via the IDF component manager (idf_component.yml in main/)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(osdp_leaf_reader)
