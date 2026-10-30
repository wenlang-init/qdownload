cmake_minimum_required(VERSION 3.21)

project(qdownload VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS
    Quick QuickControls2 Network Sql Concurrent Svg LabsPlatform LinguistTools
)

qt_standard_project_setup()

# QML 文件分布在 qml/pages、qml/components 子目录，需要为这些目录生成 qmldir，
# 否则运行时按目录解析类型会失败（QTP0004）。
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

qt_add_executable(qdownload
    src/main.cpp
)

# 必须在 qt_add_qml_module 之前设置：Theme.qml 以 pragma Singleton 注册，
# 该属性只在文件被模块消费前生效。
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(qdownload
    URI Qdownload
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/components/ToggleChip.qml
        qml/components/WriteToggle.qml
        qml/components/PeerSelector.qml
        qml/components/Breadcrumb.qml
        qml/components/EmptyState.qml
        qml/components/FileListView.qml
        qml/components/TransferDrawer.qml
        qml/components/PairCodeDialog.qml
        qml/components/PairRequestDialog.qml
        qml/components/TrustedDevicesDialog.qml
        qml/components/TrayIcon.qml
        qml/components/Toast.qml
        qml/components/ConfirmDialog.qml
        qml/components/DirPicker.qml
        qml/components/DirBrowser.qml
        qml/pages/LocalPage.qml
        qml/pages/RemotePage.qml
    RESOURCES
        # 托盘图标：SVG 交给 Qt6::Svg 渲染，系统按 DPI 缩放不会糊。
        # 必须挂在 qml/ 下面：QML 里用相对路径 ../assets/... 取图，
        # 而 qt_add_qml_module 会把资源铺到 <前缀>/包名/ 底下（qml/Main.qml 落在
        # qml/Main.qml，resources 里的路径也整体平移）。放成 assets/icons/tray.svg
        # 的话实际在 <前缀>/assets/... ，相对路径解析出来是 <前缀>/qml/assets/...，
        # 两边对不上 —— 于是图标静默加载失败，托盘什么都不显示。
        qml/assets/icons/tray.svg
    SOURCES
        src/app/AppController.h
        src/app/AppController.cpp
        src/app/LaunchOptions.h
        src/app/Logger.h
        src/app/Logger.cpp
        src/app/DataPaths.h
        src/app/DataPaths.cpp
        src/app/InstanceGuard.h
        src/app/InstanceGuard.cpp
        src/app/cxxlog.h
        src/app/cxxlog.cpp
        src/app/SystemNotifier.h
        src/app/SystemNotifier.cpp
        src/app/Translation.h
        src/app/Translation.cpp
        src/app/AndroidService.h
        src/app/AndroidService.cpp
        src/app/AndroidStorage.h
        src/app/AndroidStorage.cpp
        src/net/PeerInfo.h
        src/net/PeerInfo.cpp
        src/net/PeerListModel.h
        src/net/PeerListModel.cpp
        src/net/DiscoveryService.h
        src/net/DiscoveryService.cpp
        src/net/MessageCodec.h
        src/net/MessageCodec.cpp
        src/net/CryptoUtil.h
        src/net/CryptoUtil.cpp
        src/net/TrustStore.h
        src/net/TrustStore.cpp
        src/net/PairingService.h
        src/net/PairingService.cpp
        src/net/HostContext.h
        src/net/PeerSession.h
        src/net/PeerSession.cpp
        src/net/ControlServer.h
        src/net/ControlServer.cpp
        src/net/ControlClient.h
        src/net/ControlClient.cpp
        src/net/DataServer.h
        src/net/DataServer.cpp
        src/net/ChecksumCache.h
        src/net/ChecksumCache.cpp
        src/net/TransferGrant.h
        src/transfer/TransferTypes.h
        src/transfer/TransferTypes.cpp
        src/transfer/TransferTask.h
        src/transfer/TransferTask.cpp
        src/transfer/TransferSegment.h
        src/transfer/TransferSegment.cpp
        src/transfer/RateLimiter.h
        src/transfer/RateLimiter.cpp
        src/transfer/TransferModel.h
        src/transfer/TransferModel.cpp
        src/transfer/TransferManager.h
        src/transfer/TransferManager.cpp
        src/transfer/ResumeStore.h
        src/transfer/ResumeStore.cpp
        src/fs/FileEntry.h
        src/fs/FileEntry.cpp
        src/fs/SafePath.h
        src/fs/SafePath.cpp
        src/fs/FileScanner.h
        src/fs/FileScanner.cpp
        src/fs/FileListModel.h
        src/fs/FileListModel.cpp
)

# QMessageLogContext 获取日志上下文信息，包含文件名、行号、函数名等信息。
# 定义 QT_MESSAGELOGCONTEXT 可以启用这些功能。
add_compile_definitions(DEFINES QT_MESSAGELOGCONTEXT)

# qt_add_qml_module 生成的类型注册文件用 `__has_include(<AppController.h>)` 探测头文件，
# 因此头文件所在目录必须在 include path 里，否则 QML 类型不会被注册。
target_include_directories(qdownload PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/app
)

target_compile_definitions(qdownload PRIVATE
    QDOWNLOAD_VERSION="${PROJECT_VERSION}"
    QDOWNLOAD_BUILD_DATE=__DATE__
)

target_link_libraries(qdownload PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Network
    Qt6::Sql
    Qt6::Concurrent
    Qt6::Svg
    Qt6::LabsPlatform
)

if(ANDROID)
    # QNativeInterface 是 QtCore 提供的平台接口头文件（QtCore/qcoreapplication_platform.h），
    # 与 Qt6::Gui / GuiPrivate 无关 —— 头文件找不到时加 Gui 是没用的，要改 include 路径。
    # 这里仍然显式链接 Gui：Qt6::Quick 本来就依赖它，写出来是为了避免以后有人误删。
    find_package(Qt6 REQUIRED COMPONENTS Gui)
    target_link_libraries(qdownload PRIVATE
        Qt6::Gui
    )
endif()

# M1 阶段保留控制台，方便看日志与 --self-test 输出；打包阶段再改为 WIN32_EXECUTABLE
set_target_properties(qdownload PROPERTIES
    WIN32_EXECUTABLE TRUE #FALSE
    MACOSX_BUNDLE FALSE
)

# 三平台产物在 M8 统一处理，这里先留出 Android 包模板位置
if(ANDROID)
    set_target_properties(qdownload PROPERTIES
        QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/android"
        # 包名必须与 android/src/ 下 Java 文件的 package 声明、Manifest 里的
        # service android:name 三者完全一致，否则前台服务起不来
        QT_ANDROID_PACKAGE_NAME "org.qtproject.example.qdownload"
        # 启动器里显示的名字（不设的话会用包名或项目名，中文/空格容易出问题）
        QT_ANDROID_APP_NAME "qdownload"
        # SDK 版本必须走这两个属性：AGP 9 起 manifest 里再写 <uses-sdk> 会直接让
        # manifest merger 失败。下限 28 是 Qt 6.12 的硬要求（低于它 androiddeployqt
        # 直接报 "minSdkVersion must be >= 28"）；target 跟编译 SDK（android-36-ext19）
        # 对齐，不要写成没装过的 37，否则 AGP 会去联网下载平台。
        QT_ANDROID_MIN_SDK_VERSION 28
        QT_ANDROID_TARGET_SDK_VERSION 36
    )

    # 签名：默认出**未签名**的 release 包（装不上真机，仅供内部验证）。
    # 想装到设备上就给 keystore，tools/package_android.sh 会把下面几个变量
    # 从环境变量转手传进来（密钥不进仓库，只在 CI 的 secret 里）。
    if(QT_ANDROID_KEYSTORE_PATH)
        set_target_properties(qdownload PROPERTIES
            QT_ANDROID_SIGN_APK TRUE
            QT_ANDROID_KEYSTORE_PATH "${QT_ANDROID_KEYSTORE_PATH}"
            QT_ANDROID_KEYSTORE_ALIAS "${QT_ANDROID_KEYSTORE_ALIAS}"
            QT_ANDROID_KEYSTORE_STORE_PASS "${QT_ANDROID_KEYSTORE_STORE_PASS}"
            QT_ANDROID_KEYSTORE_KEY_PASS "${QT_ANDROID_KEYSTORE_KEY_PASS}"
        )
    endif()
endif()

# 翻译：lrelease 把 ts 编成 qm 并嵌进 qrc（:/i18n/qdownload_en.qm）。
# 中文是源语言，不需要 ts；新增/修改文案后跑：
#   lupdate src qml -ts i18n/qdownload_en.ts -locations none -no-obsolete
#   python tools/gen_en_ts.py
qt_add_translations(qdownload
    TS_FILES i18n/qdownload_en.ts
    RESOURCE_PREFIX "/i18n"
)

# 安装规则：只给 Linux 桌面打包用（tools/package_linux.sh 会
# `cmake --install build --prefix <AppDir>/usr`，再由脚本补 Qt 运行时）。
# Windows 走 windeployqt 不 install；Android 由 androiddeployqt 自己打包。
if(UNIX AND NOT ANDROID AND NOT APPLE)
    include(GNUInstallDirs)
    install(TARGETS qdownload RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES packaging/linux/qdownload.desktop
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
    install(FILES assets/icons/qdownload.svg
            DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)
endif()
