cmake_minimum_required(VERSION 3.21)

project(FacilityStateLedgerDownstream LANGUAGES CXX)

# This project is deliberately outside the Facility State Ledger source tree and
# consumes only the installed package. It is the distributability proof: an
# in-tree build is not evidence that the exported package works.

find_package(FacilityStateLedger 1.0 REQUIRED)

add_executable(downstream_consumer main.cpp)
target_link_libraries(downstream_consumer PRIVATE FacilityStateLedger::facility_state_ledger)
set_target_properties(downstream_consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
