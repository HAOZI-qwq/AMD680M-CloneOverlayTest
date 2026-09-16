# GitHub Actions 在线编译

仓库已配置 `.github/workflows/build-windows.yml`。

每次向 `main` 推送 `src/**`、`CMakeLists.txt` 或 workflow 文件时，GitHub 会使用 `windows-latest` + Visual Studio 2022 自动编译 x64 Release。

构建成功后，在对应 Actions 运行页面底部下载 Artifact：

`AMDCloneOverlayProbe-Windows-x64`

解压后直接运行 `AMDCloneOverlayProbe.exe`，本机不需要安装 Visual Studio、CMake 或 Windows SDK。
