# Copyright 2020-2026 Yonik Seeley and Luxir contributors
# SPDX-License-Identifier: Apache-2.0

luxir_add_plugin(NAME example
                 SOURCES ExamplePlugin.cpp
                 TEST_SOURCES PluginExtensionTest.cpp StorageExampleTest.cpp
                 TEST_ONLY)
