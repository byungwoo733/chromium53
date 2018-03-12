// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/installer/setup/install.h"

#include <objbase.h>
#include <stddef.h>

#include <memory>
#include <string>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/macros.h"
#include "base/path_service.h"
#include "base/strings/string16.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/test/scoped_path_override.h"
#include "base/test/test_shortcut_win.h"
#include "base/version.h"
#include "base/win/shortcut.h"
#include "chrome/installer/setup/install_worker.h"
#include "chrome/installer/setup/setup_constants.h"
#include "chrome/installer/util/browser_distribution.h"
#include "chrome/installer/util/install_util.h"
#include "chrome/installer/util/installer_state.h"
#include "chrome/installer/util/master_preferences.h"
#include "chrome/installer/util/master_preferences_constants.h"
#include "chrome/installer/util/product.h"
#include "chrome/installer/util/shell_util.h"
#include "chrome/installer/util/util_constants.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace {

base::FilePath GetNormalizedFilePath(const base::FilePath& path) {
  base::FilePath normalized_path;
  EXPECT_TRUE(base::NormalizeFilePath(path, &normalized_path));
  return normalized_path;
}

class CreateVisualElementsManifestTest : public testing::Test {
 protected:
  void SetUp() override {
    // Create a temp directory for testing.
    ASSERT_TRUE(test_dir_.CreateUniqueTempDir());

    version_ = Version("0.0.0.0");

    version_dir_ = test_dir_.path().AppendASCII(version_.GetString());
    ASSERT_TRUE(base::CreateDirectory(version_dir_));

    manifest_path_ =
        test_dir_.path().Append(installer::kVisualElementsManifest);
  }

  void TearDown() override {
    // Clean up test directory manually so we can fail if it leaks.
    ASSERT_TRUE(test_dir_.Delete());
  }

  // The temporary directory used to contain the test operations.
  base::ScopedTempDir test_dir_;

  // A dummy version number used to create the version directory.
  Version version_;

  // The path to |test_dir_|\|version_|.
  base::FilePath version_dir_;

  // The path to VisualElementsManifest.xml.
  base::FilePath manifest_path_;
};

class InstallShortcutTest : public testing::Test {
 protected:
  struct UpdateShortcutsTestCase {
    // Shortcut target path, relative to |temp_dir_|.
    const base::FilePath::CharType* target_path;

    // Shortcut icon path, relative to |temp_dir_|. Can be null to create a
    // shortcut without an icon.
    const base::FilePath::CharType* icon_path;

    // Whether the shortcut's target path should be updated by
    // UpdatePerUserShortcutsInLocation().
    bool should_update;
  };

  void SetUp() override {
    EXPECT_EQ(S_OK, CoInitialize(NULL));

    dist_ = BrowserDistribution::GetDistribution();
    ASSERT_TRUE(dist_ != NULL);
    product_.reset(new installer::Product(dist_));

    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    chrome_exe_ = temp_dir_.path().Append(installer::kChromeExe);
    EXPECT_EQ(0, base::WriteFile(chrome_exe_, "", 0));

    ShellUtil::ShortcutProperties chrome_properties(ShellUtil::CURRENT_USER);
    product_->AddDefaultShortcutProperties(chrome_exe_, &chrome_properties);

    expected_properties_.set_target(chrome_exe_);
    expected_properties_.set_icon(chrome_properties.icon,
                                  chrome_properties.icon_index);
    expected_properties_.set_app_id(chrome_properties.app_id);
    expected_properties_.set_description(chrome_properties.description);
    expected_properties_.set_dual_mode(false);
    expected_start_menu_properties_ = expected_properties_;
    expected_start_menu_properties_.set_dual_mode(false);

    prefs_.reset(GetFakeMasterPrefs(false, false));

    ASSERT_TRUE(fake_user_desktop_.CreateUniqueTempDir());
    ASSERT_TRUE(fake_common_desktop_.CreateUniqueTempDir());
    ASSERT_TRUE(fake_user_quick_launch_.CreateUniqueTempDir());
    ASSERT_TRUE(fake_start_menu_.CreateUniqueTempDir());
    ASSERT_TRUE(fake_common_start_menu_.CreateUniqueTempDir());
    user_desktop_override_.reset(
        new base::ScopedPathOverride(base::DIR_USER_DESKTOP,
                                     fake_user_desktop_.path()));
    common_desktop_override_.reset(
        new base::ScopedPathOverride(base::DIR_COMMON_DESKTOP,
                                     fake_common_desktop_.path()));
    user_quick_launch_override_.reset(
        new base::ScopedPathOverride(base::DIR_USER_QUICK_LAUNCH,
                                     fake_user_quick_launch_.path()));
    start_menu_override_.reset(
        new base::ScopedPathOverride(base::DIR_START_MENU,
                                     fake_start_menu_.path()));
    common_start_menu_override_.reset(
        new base::ScopedPathOverride(base::DIR_COMMON_START_MENU,
                                     fake_common_start_menu_.path()));

    base::string16 shortcut_name(dist_->GetShortcutName() + installer::kLnkExt);

    user_desktop_shortcut_ =
        fake_user_desktop_.path().Append(shortcut_name);
    user_quick_launch_shortcut_ =
        fake_user_quick_launch_.path().Append(shortcut_name);
    user_start_menu_shortcut_ = fake_start_menu_.path().Append(shortcut_name);
    user_start_menu_subdir_shortcut_ =
        fake_start_menu_.path()
            .Append(dist_->GetStartMenuShortcutSubfolder(
                BrowserDistribution::SUBFOLDER_CHROME))
            .Append(shortcut_name);
    system_desktop_shortcut_ =
        fake_common_desktop_.path().Append(shortcut_name);
    system_start_menu_shortcut_ =
        fake_common_start_menu_.path().Append(shortcut_name);
    system_start_menu_subdir_shortcut_ =
        fake_common_start_menu_.path()
            .Append(dist_->GetStartMenuShortcutSubfolder(
                BrowserDistribution::SUBFOLDER_CHROME))
            .Append(shortcut_name);
  }

  void TearDown() override {
    // Try to unpin potentially pinned shortcuts (although pinning isn't tested,
    // the call itself might still have pinned the Start Menu shortcuts).
    base::win::UnpinShortcutFromTaskbar(user_start_menu_shortcut_);
    base::win::UnpinShortcutFromTaskbar(user_start_menu_subdir_shortcut_);
    base::win::UnpinShortcutFromTaskbar(system_start_menu_shortcut_);
    base::win::UnpinShortcutFromTaskbar(system_start_menu_subdir_shortcut_);
    CoUninitialize();
  }

  installer::MasterPreferences* GetFakeMasterPrefs(
      bool do_not_create_desktop_shortcut,
      bool do_not_create_quick_launch_shortcut) {
    const struct {
      const char* pref_name;
      bool is_desired;
    } desired_prefs[] = {
      { installer::master_preferences::kDoNotCreateDesktopShortcut,
        do_not_create_desktop_shortcut },
      { installer::master_preferences::kDoNotCreateQuickLaunchShortcut,
        do_not_create_quick_launch_shortcut },
    };

    std::string master_prefs("{\"distribution\":{");
    for (size_t i = 0; i < arraysize(desired_prefs); ++i) {
      master_prefs += (i == 0 ? "\"" : ",\"");
      master_prefs += desired_prefs[i].pref_name;
      master_prefs += "\":";
      master_prefs += desired_prefs[i].is_desired ? "true" : "false";
    }
    master_prefs += "}}";

    return new installer::MasterPreferences(master_prefs);
  }

  // Creates the shortcuts defined by |test_cases|. Tries to update the target
  // path of these shortcuts to |new_target_path_relative| using
  // UpdatePerUserShortcutsInLocation(). Verifies that the right shortcuts have
  // been updated.
  void TestUpdateShortcuts(const UpdateShortcutsTestCase* test_cases,
                           size_t num_test_cases,
                           const base::FilePath& new_target_path_relative) {
    // Create shortcuts.
    for (size_t i = 0; i < num_test_cases; ++i) {
      // Make sure that the target exists.
      const base::FilePath target_path =
          temp_dir_.path().Append(test_cases[i].target_path);
      if (!base::PathExists(target_path)) {
        ASSERT_TRUE(base::CreateDirectory(target_path.DirName()));
        base::File file(target_path, base::File::FLAG_CREATE_ALWAYS |
                                         base::File::FLAG_WRITE);
        ASSERT_TRUE(file.IsValid());
        static const char kDummyData[] = "dummy";
        ASSERT_EQ(arraysize(kDummyData),
                  static_cast<size_t>(file.WriteAtCurrentPos(
                      kDummyData, arraysize(kDummyData))));
      }

      // Create the shortcut.
      base::win::ShortcutProperties properties;
      properties.set_target(target_path);
      properties.set_icon(temp_dir_.path().Append(test_cases[i].icon_path), 1);
      ASSERT_TRUE(base::win::CreateOrUpdateShortcutLink(
          user_desktop_shortcut_.InsertBeforeExtension(
              base::SizeTToString16(i)),
          properties, base::win::SHORTCUT_CREATE_ALWAYS));
    }

    // Update shortcuts.
    const base::FilePath new_target_path =
        temp_dir_.path().Append(new_target_path_relative);
    installer::UpdatePerUserShortcutsInLocation(
        ShellUtil::SHORTCUT_LOCATION_DESKTOP, dist_,
        new_target_path.DirName().DirName(), new_target_path.BaseName(),
        new_target_path);

    // Verify that shortcuts were updated correctly.
    for (size_t i = 0; i < num_test_cases; ++i) {
      base::FilePath target_path;
      ASSERT_TRUE(base::win::ResolveShortcut(
          user_desktop_shortcut_.InsertBeforeExtension(
              base::SizeTToString16(i)),
          &target_path, nullptr));

      if (test_cases[i].should_update) {
        EXPECT_EQ(GetNormalizedFilePath(new_target_path),
                  GetNormalizedFilePath(target_path));
      } else {
        EXPECT_EQ(GetNormalizedFilePath(
                      temp_dir_.path().Append(test_cases[i].target_path)),
                  GetNormalizedFilePath(target_path));
      }
    }
  }

  base::win::ShortcutProperties expected_properties_;
  base::win::ShortcutProperties expected_start_menu_properties_;

  BrowserDistribution* dist_;
  base::FilePath chrome_exe_;
  std::unique_ptr<installer::Product> product_;
  std::unique_ptr<installer::MasterPreferences> prefs_;

  base::ScopedTempDir temp_dir_;
  base::ScopedTempDir fake_user_desktop_;
  base::ScopedTempDir fake_common_desktop_;
  base::ScopedTempDir fake_user_quick_launch_;
  base::ScopedTempDir fake_start_menu_;
  base::ScopedTempDir fake_common_start_menu_;
  std::unique_ptr<base::ScopedPathOverride> user_desktop_override_;
  std::unique_ptr<base::ScopedPathOverride> common_desktop_override_;
  std::unique_ptr<base::ScopedPathOverride> user_quick_launch_override_;
  std::unique_ptr<base::ScopedPathOverride> start_menu_override_;
  std::unique_ptr<base::ScopedPathOverride> common_start_menu_override_;

  base::FilePath user_desktop_shortcut_;
  base::FilePath user_quick_launch_shortcut_;
  base::FilePath user_start_menu_shortcut_;
  base::FilePath user_start_menu_subdir_shortcut_;
  base::FilePath system_desktop_shortcut_;
  base::FilePath system_start_menu_shortcut_;
  base::FilePath system_start_menu_subdir_shortcut_;
};

}  // namespace

// Test that VisualElementsManifest.xml is not created when VisualElements are
// not present.
TEST_F(CreateVisualElementsManifestTest, VisualElementsManifestNotCreated) {
  ASSERT_TRUE(
      installer::CreateVisualElementsManifest(test_dir_.path(), version_));
  ASSERT_FALSE(base::PathExists(manifest_path_));
}

// Test that VisualElementsManifest.xml is created with the correct content when
// VisualElements are present.
TEST_F(CreateVisualElementsManifestTest, VisualElementsManifestCreated) {
  ASSERT_TRUE(base::CreateDirectory(
      version_dir_.Append(installer::kVisualElements)));
  ASSERT_TRUE(
      installer::CreateVisualElementsManifest(test_dir_.path(), version_));
  ASSERT_TRUE(base::PathExists(manifest_path_));

  std::string read_manifest;
  ASSERT_TRUE(base::ReadFileToString(manifest_path_, &read_manifest));

  static const char kExpectedManifest[] =
      "<Application xmlns:xsi='http://www.w3.org/2001/XMLSchema-instance'>\r\n"
      "  <VisualElements\r\n"
      "      ShowNameOnSquare150x150Logo='on'\r\n"
      "      Square150x150Logo='0.0.0.0\\VisualElements\\Logo.png'\r\n"
      "      Square70x70Logo='0.0.0.0\\VisualElements\\SmallLogo.png'\r\n"
      "      Square44x44Logo='0.0.0.0\\VisualElements\\SmallLogo.png'\r\n"
      "      ForegroundText='light'\r\n"
      "      BackgroundColor='#212121'/>\r\n"
      "</Application>\r\n";

  ASSERT_STREQ(kExpectedManifest, read_manifest.c_str());
}

TEST_F(InstallShortcutTest, CreateAllShortcuts) {
  installer::CreateOrUpdateShortcuts(
      chrome_exe_, *product_, *prefs_, installer::CURRENT_USER,
      installer::INSTALL_SHORTCUT_CREATE_ALL);
  base::win::ValidateShortcut(user_desktop_shortcut_, expected_properties_);
  base::win::ValidateShortcut(user_quick_launch_shortcut_,
                              expected_properties_);
  base::win::ValidateShortcut(user_start_menu_shortcut_,
                              expected_start_menu_properties_);
}

TEST_F(InstallShortcutTest, CreateAllShortcutsSystemLevel) {
  installer::CreateOrUpdateShortcuts(
      chrome_exe_, *product_, *prefs_, installer::ALL_USERS,
      installer::INSTALL_SHORTCUT_CREATE_ALL);
  base::win::ValidateShortcut(system_desktop_shortcut_, expected_properties_);
  base::win::ValidateShortcut(system_start_menu_shortcut_,
                              expected_start_menu_properties_);
  // The quick launch shortcut is always created per-user for the admin running
  // the install (other users will get it via Active Setup).
  base::win::ValidateShortcut(user_quick_launch_shortcut_,
                              expected_properties_);
}

TEST_F(InstallShortcutTest, CreateAllShortcutsButDesktopShortcut) {
  std::unique_ptr<installer::MasterPreferences> prefs_no_desktop(
      GetFakeMasterPrefs(true, false));
  installer::CreateOrUpdateShortcuts(
      chrome_exe_, *product_, *prefs_no_desktop, installer::CURRENT_USER,
      installer::INSTALL_SHORTCUT_CREATE_ALL);
  ASSERT_FALSE(base::PathExists(user_desktop_shortcut_));
  base::win::ValidateShortcut(user_quick_launch_shortcut_,
                              expected_properties_);
  base::win::ValidateShortcut(user_start_menu_shortcut_,
                              expected_start_menu_properties_);
}

TEST_F(InstallShortcutTest, CreateAllShortcutsButQuickLaunchShortcut) {
  std::unique_ptr<installer::MasterPreferences> prefs_no_ql(
      GetFakeMasterPrefs(false, true));
  installer::CreateOrUpdateShortcuts(
      chrome_exe_, *product_, *prefs_no_ql, installer::CURRENT_USER,
      installer::INSTALL_SHORTCUT_CREATE_ALL);
  base::win::ValidateShortcut(user_desktop_shortcut_, expected_properties_);
  ASSERT_FALSE(base::PathExists(user_quick_launch_shortcut_));
  base::win::ValidateShortcut(user_start_menu_shortcut_,
                              expected_start_menu_properties_);
}

TEST_F(InstallShortcutTest, ReplaceAll) {
  base::win::ShortcutProperties dummy_properties;
  base::FilePath dummy_target;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(), &dummy_target));
  dummy_properties.set_target(dummy_target);
  dummy_properties.set_working_dir(fake_user_desktop_.path());
  dummy_properties.set_arguments(L"--dummy --args");
  dummy_properties.set_app_id(L"El.Dummiest");

  ASSERT_TRUE(base::win::CreateOrUpdateShortcutLink(
                  user_desktop_shortcut_, dummy_properties,
                  base::win::SHORTCUT_CREATE_ALWAYS));
  ASSERT_TRUE(base::win::CreateOrUpdateShortcutLink(
                  user_quick_launch_shortcut_, dummy_properties,
                  base::win::SHORTCUT_CREATE_ALWAYS));
  ASSERT_TRUE(base::CreateDirectory(user_start_menu_shortcut_.DirName()));
  ASSERT_TRUE(base::win::CreateOrUpdateShortcutLink(
                  user_start_menu_shortcut_, dummy_properties,
                  base::win::SHORTCUT_CREATE_ALWAYS));

  installer::CreateOrUpdateShortcuts(
      chrome_exe_, *product_, *prefs_, installer::CURRENT_USER,
      installer::INSTALL_SHORTCUT_REPLACE_EXISTING);
  base::win::ValidateShortcut(user_desktop_shortcut_, expected_properties_);
  base::win::ValidateShortcut(user_quick_launch_shortcut_,
                              expected_properties_);
  base::win::ValidateShortcut(user_start_menu_shortcut_,
                              expected_start_menu_properties_);
}

TEST_F(InstallShortcutTest, ReplaceExisting) {
  base::win::ShortcutProperties dummy_properties;
  base::FilePath dummy_target;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(), &dummy_target));
  dummy_properties.set_target(dummy_target);
  dummy_properties.set_working_dir(fake_user_desktop_.path());
  dummy_properties.set_arguments(L"--dummy --args");
  dummy_properties.set_app_id(L"El.Dummiest");

  ASSERT_TRUE(base::win::CreateOrUpdateShortcutLink(
                  user_desktop_shortcut_, dummy_properties,
                  base::win::SHORTCUT_CREATE_ALWAYS));
  ASSERT_TRUE(base::CreateDirectory(user_start_menu_shortcut_.DirName()));

  installer::CreateOrUpdateShortcuts(
      chrome_exe_, *product_, *prefs_, installer::CURRENT_USER,
      installer::INSTALL_SHORTCUT_REPLACE_EXISTING);
  base::win::ValidateShortcut(user_desktop_shortcut_, expected_properties_);
  ASSERT_FALSE(base::PathExists(user_quick_launch_shortcut_));
  ASSERT_FALSE(base::PathExists(user_start_menu_shortcut_));
}

class MigrateShortcutTest : public InstallShortcutTest,
                            public testing::WithParamInterface<
                                testing::tuple<
                                    installer::InstallShortcutOperation,
                                    installer::InstallShortcutLevel>> {
 public:
  MigrateShortcutTest() : shortcut_operation_(testing::get<0>(GetParam())),
                          shortcut_level_(testing::get<1>(GetParam())) {}

 protected:
  const installer::InstallShortcutOperation shortcut_operation_;
  const installer::InstallShortcutLevel shortcut_level_;

 private:
  DISALLOW_COPY_AND_ASSIGN(MigrateShortcutTest);
};

TEST_P(MigrateShortcutTest, MigrateAwayFromDeprecatedStartMenuTest) {
  base::win::ShortcutProperties dummy_properties;
  base::FilePath dummy_target;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(), &dummy_target));
  dummy_properties.set_target(expected_properties_.target);
  dummy_properties.set_working_dir(fake_user_desktop_.path());
  dummy_properties.set_arguments(L"--dummy --args");
  dummy_properties.set_app_id(L"El.Dummiest");

  base::FilePath start_menu_shortcut;
  base::FilePath start_menu_subdir_shortcut;
  if (shortcut_level_ == installer::CURRENT_USER) {
    start_menu_shortcut = user_start_menu_shortcut_;
    start_menu_subdir_shortcut = user_start_menu_subdir_shortcut_;
  } else {
    start_menu_shortcut = system_start_menu_shortcut_;
    start_menu_subdir_shortcut = system_start_menu_subdir_shortcut_;
  }

  ASSERT_TRUE(base::CreateDirectory(start_menu_subdir_shortcut.DirName()));
  ASSERT_FALSE(base::PathExists(start_menu_subdir_shortcut));
  ASSERT_TRUE(base::win::CreateOrUpdateShortcutLink(
                  start_menu_subdir_shortcut, dummy_properties,
                  base::win::SHORTCUT_CREATE_ALWAYS));
  ASSERT_TRUE(base::PathExists(start_menu_subdir_shortcut));
  ASSERT_FALSE(base::PathExists(start_menu_shortcut));

  installer::CreateOrUpdateShortcuts(chrome_exe_, *product_, *prefs_,
                                     shortcut_level_, shortcut_operation_);
  ASSERT_FALSE(base::PathExists(start_menu_subdir_shortcut));
  ASSERT_TRUE(base::PathExists(start_menu_shortcut));
}

// Verify that any installer operation for any installation level triggers
// the migration from sub-folder to root of start-menu.
INSTANTIATE_TEST_CASE_P(
    MigrateShortcutTests, MigrateShortcutTest,
    testing::Combine(
        testing::Values(
            installer::INSTALL_SHORTCUT_REPLACE_EXISTING,
            installer::INSTALL_SHORTCUT_CREATE_EACH_IF_NO_SYSTEM_LEVEL,
            installer::INSTALL_SHORTCUT_CREATE_ALL),
        testing::Values(
            installer::CURRENT_USER,
            installer::ALL_USERS)));

TEST_F(InstallShortcutTest, CreateIfNoSystemLevelAllSystemShortcutsExist) {
  base::win::ShortcutProperties dummy_properties;
  base::FilePath dummy_target;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(), &dummy_target));
  dummy_properties.set_target(dummy_target);

  ASSERT_TRUE(base::win::CreateOrUpdateShortcutLink(
                  system_desktop_shortcut_, dummy_properties,
                  base::win::SHORTCUT_CREATE_ALWAYS));
  ASSERT_TRUE(base::CreateDirectory(
        system_start_menu_shortcut_.DirName()));
  ASSERT_TRUE(base::win::CreateOrUpdateShortcutLink(
                  system_start_menu_shortcut_, dummy_properties,
                  base::win::SHORTCUT_CREATE_ALWAYS));

  installer::CreateOrUpdateShortcuts(
      chrome_exe_, *product_, *prefs_, installer::CURRENT_USER,
      installer::INSTALL_SHORTCUT_CREATE_EACH_IF_NO_SYSTEM_LEVEL);
  ASSERT_FALSE(base::PathExists(user_desktop_shortcut_));
  ASSERT_FALSE(base::PathExists(user_start_menu_shortcut_));
  // There is no system-level quick launch shortcut, so creating the user-level
  // one should always succeed.
  ASSERT_TRUE(base::PathExists(user_quick_launch_shortcut_));
}

TEST_F(InstallShortcutTest, CreateIfNoSystemLevelNoSystemShortcutsExist) {
  installer::CreateOrUpdateShortcuts(
      chrome_exe_, *product_, *prefs_, installer::CURRENT_USER,
      installer::INSTALL_SHORTCUT_CREATE_EACH_IF_NO_SYSTEM_LEVEL);
  base::win::ValidateShortcut(user_desktop_shortcut_, expected_properties_);
  base::win::ValidateShortcut(user_quick_launch_shortcut_,
                              expected_properties_);
  base::win::ValidateShortcut(user_start_menu_shortcut_,
                              expected_start_menu_properties_);
}

TEST_F(InstallShortcutTest, CreateIfNoSystemLevelSomeSystemShortcutsExist) {
  base::win::ShortcutProperties dummy_properties;
  base::FilePath dummy_target;
  ASSERT_TRUE(base::CreateTemporaryFileInDir(temp_dir_.path(), &dummy_target));
  dummy_properties.set_target(dummy_target);

  ASSERT_TRUE(base::win::CreateOrUpdateShortcutLink(
                  system_desktop_shortcut_, dummy_properties,
                  base::win::SHORTCUT_CREATE_ALWAYS));

  installer::CreateOrUpdateShortcuts(
      chrome_exe_, *product_, *prefs_, installer::CURRENT_USER,
      installer::INSTALL_SHORTCUT_CREATE_EACH_IF_NO_SYSTEM_LEVEL);
  ASSERT_FALSE(base::PathExists(user_desktop_shortcut_));
  base::win::ValidateShortcut(user_quick_launch_shortcut_,
                              expected_properties_);
  base::win::ValidateShortcut(user_start_menu_shortcut_,
                              expected_start_menu_properties_);
}

TEST_F(InstallShortcutTest, UpdatePerUserChromeUserLevelShortcuts) {
  static const UpdateShortcutsTestCase kTestCases[] = {
      // Shortcut target in the Chrome Canary install directory. No icon.
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Temp\\scoped_dir\\new_chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Temp\\scoped_dir\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\something_else.exe"),
       nullptr, false},

      // Shortcut target in the user-level Chrome install directory. No icon.
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome\\Temp\\scope"
                         "d_dir\\new_chrome.exe"),
       nullptr, true},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome\\Temp\\scope"
                         "d_dir\\chrome.exe"),
       nullptr, true},
      {FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe"),
       nullptr, true},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome\\Application"
                         "\\something_else.exe"),
       nullptr, false},

      // Shortcut target in the system-level Chrome install directory. No icon.
      {FILE_PATH_LITERAL("Program Files "
                         "(x86)\\Google\\Chrome\\Temp\\scoped_dir\\new_chrome."
                         "exe"),
       nullptr, false},
      {FILE_PATH_LITERAL(
           "Program Files (x86)\\Google\\Chrome\\Temp\\scoped_dir\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL(
           "Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Program Files "
                         "(x86)\\Google\\Chrome\\Application\\something_else."
                         "exe"),
       nullptr, false},

      // Dummy shortcut target. Icon in the Chrome Canary install directory.
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\chrome.exe"),
       false},
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\User Data\\Profile 1\\Google "
                         "Profile.ico"),
       false},

      // Dummy shortcut target. Icon in the user-level Chrome install directory.
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe"),
       true},
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Chrome\\Application\\User "
           "Data\\Profile 1\\Google Profile.ico"),
       true},

      // Dummy shortcut target. Icon in the system-level Chrome install
      // directory.
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL(
           "Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe"),
       false},

      // Shortcuts that don't belong to Chrome.
      {FILE_PATH_LITERAL("something_else.exe"), nullptr, false},
      {FILE_PATH_LITERAL("something_else.exe"),
       FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Something Else.ico"),
       false},
  };

  TestUpdateShortcuts(
      kTestCases, arraysize(kTestCases),
      base::FilePath(FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrom"
                                       "e\\Application\\chrome.exe")));
}

TEST_F(InstallShortcutTest, UpdatePerUserCanaryShortcuts) {
  static const UpdateShortcutsTestCase kTestCases[] = {
      // Shortcut target in the Chrome Canary install directory. No icon.
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Temp\\scoped_dir\\new_chrome.exe"),
       nullptr, true},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Temp\\scoped_dir\\chrome.exe"),
       nullptr, true},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\chrome.exe"),
       nullptr, true},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\something_else.exe"),
       nullptr, false},

      // Shortcut target in the user-level Chrome install directory. No icon.
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome\\Temp\\scope"
                         "d_dir\\new_chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome\\Temp\\scope"
                         "d_dir\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome\\Application"
                         "\\something_else.exe"),
       nullptr, false},

      // Shortcut target in the system-level Chrome install directory. No icon.
      {FILE_PATH_LITERAL("Program Files "
                         "(x86)\\Google\\Chrome\\Temp\\scoped_dir\\new_chrome."
                         "exe"),
       nullptr, false},
      {FILE_PATH_LITERAL(
           "Program Files (x86)\\Google\\Chrome\\Temp\\scoped_dir\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL(
           "Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Program Files "
                         "(x86)\\Google\\Chrome\\Application\\something_else."
                         "exe"),
       nullptr, false},

      // Dummy shortcut target. Icon in the Chrome Canary install directory.
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\chrome.exe"),
       true},
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\User Data\\Profile 1\\Google "
                         "Profile.ico"),
       true},

      // Dummy shortcut target. Icon in the user-level Chrome install directory.
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe"),
       false},
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Chrome\\Application\\User "
           "Data\\Profile 1\\Google Profile.ico"),
       false},

      // Dummy shortcut target. Icon in the system-level Chrome install
      // directory.
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL(
           "Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe"),
       false},

      // Shortcuts that don't belong to Chrome.
      {FILE_PATH_LITERAL("something_else.exe"), nullptr, false},
      {FILE_PATH_LITERAL("something_else.exe"),
       FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Something Else.ico"),
       false},
  };

  TestUpdateShortcuts(
      kTestCases, arraysize(kTestCases),
      base::FilePath(FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrom"
                                       "e SxS\\Application\\chrome.exe")));
}

TEST_F(InstallShortcutTest, UpdatePerUserChromeSystemLevelShortcuts) {
  static const UpdateShortcutsTestCase kTestCases[] = {
      // Shortcut target in the Chrome Canary install directory. No icon.
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Temp\\scoped_dir\\new_chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Temp\\scoped_dir\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\something_else.exe"),
       nullptr, false},

      // Shortcut target in the user-level Chrome install directory. No icon.
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome\\Temp\\scope"
                         "d_dir\\new_chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome\\Temp\\scope"
                         "d_dir\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe"),
       nullptr, false},
      {FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome\\Application"
                         "\\something_else.exe"),
       nullptr, false},

      // Shortcut target in the system-level Chrome install directory. No icon.
      {FILE_PATH_LITERAL("Program Files "
                         "(x86)\\Google\\Chrome\\Temp\\scoped_dir\\new_chrome."
                         "exe"),
       nullptr, true},
      {FILE_PATH_LITERAL(
           "Program Files (x86)\\Google\\Chrome\\Temp\\scoped_dir\\chrome.exe"),
       nullptr, true},
      {FILE_PATH_LITERAL(
           "Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe"),
       nullptr, true},
      {FILE_PATH_LITERAL("Program Files "
                         "(x86)\\Google\\Chrome\\Application\\something_else."
                         "exe"),
       nullptr, false},

      // Dummy shortcut target. Icon in the Chrome Canary install directory.
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\chrome.exe"),
       false},
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL("Users\\x\\AppData\\Local\\Google\\Chrome "
                         "SxS\\Application\\User Data\\Profile 1\\Google "
                         "Profile.ico"),
       false},

      // Dummy shortcut target. Icon in the user-level Chrome install directory.
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Chrome\\Application\\chrome.exe"),
       false},
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Chrome\\Application\\User "
           "Data\\Profile 1\\Google Profile.ico"),
       false},

      // Dummy shortcut target. Icon in the system-level Chrome install
      // directory.
      {FILE_PATH_LITERAL("dummy.exe"),
       FILE_PATH_LITERAL(
           "Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe"),
       true},

      // Shortcuts that don't belong to Chrome.
      {FILE_PATH_LITERAL("something_else.exe"), nullptr, false},
      {FILE_PATH_LITERAL("something_else.exe"),
       FILE_PATH_LITERAL(
           "Users\\x\\AppData\\Local\\Google\\Something Else.ico"),
       false},
  };

  TestUpdateShortcuts(
      kTestCases, arraysize(kTestCases),
      base::FilePath(FILE_PATH_LITERAL(
          "Program Files (x86)\\Google\\Chrome\\Application\\chrome.exe")));
}

TEST(EscapeXmlAttributeValueTest, EscapeCrazyValue) {
  base::string16 val(L"This has 'crazy' \"chars\" && < and > signs.");
  static const wchar_t kExpectedEscapedVal[] =
      L"This has &apos;crazy&apos; \"chars\" &amp;&amp; &lt; and > signs.";
  installer::EscapeXmlAttributeValueInSingleQuotes(&val);
  ASSERT_STREQ(kExpectedEscapedVal, val.c_str());
}

TEST(EscapeXmlAttributeValueTest, DontEscapeNormalValue) {
  base::string16 val(L"Google Chrome");
  static const wchar_t kExpectedEscapedVal[] = L"Google Chrome";
  installer::EscapeXmlAttributeValueInSingleQuotes(&val);
  ASSERT_STREQ(kExpectedEscapedVal, val.c_str());
}