# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

luxir_add_plugin(NAME example
                 SOURCES ExamplePlugin.cpp
                 TEST_SOURCES PluginExtensionTest.cpp StorageExampleTest.cpp TokenAttributesExampleTest.cpp MergePolicyPluginTest.cpp
                 TEST_ONLY)
target_include_directories(luxir_plugin_example PRIVATE ../..)
target_compile_options(luxir_plugin_example PRIVATE ${LUXIR_TEST_WARNING_OPTIONS})
