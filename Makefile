#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := tractor

EXTRA_COMPONENT_DIRS := $(realpath ../..)

include $(IDF_PATH)/make/project.mk
