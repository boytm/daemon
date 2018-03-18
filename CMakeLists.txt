cmake_minimum_required (VERSION 2.6)
project (daemon)

set( CMAKE_VERBOSE_MAKEFILE on )

add_definitions( -D_UNICODE -DUNICODE -D"_WIN32_WINNT=0x0501" )
add_executable(daemon daemon.cpp ntservice.c bsd_getopt.c)
