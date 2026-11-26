# kairocopy

在 Windows 和移动硬盘之间快速拷贝文件：先分析，再拷贝；拷过的不会再拷；中断了可以接着拷。
界面像 Total Commander：左右两栏，选中一边的文件或文件夹，按 F5 拷到另一边当前的文件夹。

## 用法

- 两栏顶部选盘符，或者在路径框里直接输入路径后回车。Enter / 双击进入文件夹，Backspace 返回上一级，↑ 按钮同样是上一级。
- **Tab** 切换左右栏。当前栏的路径框是蓝底的；F5 按钮上的箭头指向拷贝的方向。
- 选择：鼠标点选，Ctrl / Shift 多选；或者用 **Space / Insert** 逐个标记（光标自动下移），**Ctrl+A** 全选。
- **F5 拷贝**：先分析选中的内容，再弹出确认框，显示共多少文件、要拷多少、跳过多少已经拷过的、目标剩余空间。
  - 可以选“全部拷贝”或“只拷重要的”（照片、视频、音乐、文档、邮件、聊天记录、代码、设计稿、压缩包）。
  - 可以勾选“拷完后逐个读回来校验”。
  - “按类别看”展开每一类的数量和大小。
- **F3 分析**：效果和 F5 一样，先分析、再确认。
- 拷贝过程中，下方显示总进度（百分比、文件数、大小、速度、剩余时间）和当前文件的进度，任务栏图标也显示进度。可以**暂停 / 继续**、**停止**。
- Ctrl+R 刷新两栏。两栏所在的文件夹会记住，下次打开时还在那里。

## 不会重复拷贝，也不会留下半个文件

- 每个文件先写成 `名字.kcpart`，写完后才改成原名；目标里不会出现拷了一半、却顶着原名的文件。
- 目标里已经有、并且大小和修改时间都一样的文件会跳过（允许 2 秒误差：exFAT / FAT 的时间精度低）。
- 源文件比目标里的新，就覆盖目标里的；目标里的更新或者不同，就不动它，在确认框里单独列出来。
- 每拷完一个文件，就记到目标盘根目录的 `.kairocopy\manifest.db`（SQLite，隐藏文件夹），每次运行也记一条。
- 停止、关掉程序、断电之后，再按一次 F5，只会拷还没拷完的。

## 跳过什么

- 系统和程序：盘符根目录下的 Windows、Program Files、ProgramData 等；pagefile.sys、hiberfil.sys；回收站；System Volume Information。
- 缓存：AppData 下的 Cache、Temp、GPUCache 等，浏览器和各种程序的缓存，以及 AppData\Local 下的 Packages、Programs 等程序目录。
- 开发产物：node_modules、__pycache__、.gradle、.vs，以及项目里的 build / obj / dist 等输出目录。
- 只在云端的文件（OneDrive 占位文件）不会去下载。Thumbs.db、desktop.ini 这类文件也不拷。
- 快捷方式文件夹（junction，比如旧版的 “My Documents”）不跟进去，避免循环。

## 速度

测试环境：USB 移动硬盘（exFAT，系统自己的 robocopy 拷大文件也只有 41 MB/s）。

| 内容 | kairocopy | robocopy | robocopy /MT:8 |
|---|---|---|---|
| 4 个 512 MB 大文件 | 40 MB/s | 41 MB/s（/J） | |
| 500 个小文件（约 200 KB） | 86 个/秒（8 个线程） | 60 个/秒 | |
| 5004 个文件，3.17 GB（大小混合） | **110 秒** | 135 秒 | 139 秒 |

做法：
- 大文件（≥ 8 MB）不经过系统缓存，直接写盘。
- 小文件用 8 个线程并行。
- 目标是机械硬盘时，先拷小文件，再一个一个拷大文件，避免磁头来回跑。
- 拷贝顺序按目标路径排，同一个文件夹的文件连着写。

## 命令行（给脚本和 agent 用）

```
kairocopy-cli analyze <源>... --to <目标文件夹> [--important] [--json]
kairocopy-cli copy <源>... --to <目标文件夹> [--important] [--verify] [--writers 大,小] [--quiet] [--json]
kairocopy-cli status <目标文件夹> [--json]
```

退出码：0 成功，1 失败或被停止，2 用法错误。Ctrl+C 停止，已经拷完的保留。

## 构建（MSYS2 UCRT64）

```
pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,sqlite3,xxhash}
cmake -S . -B build -G Ninja && cmake --build build
build/kc_tests.exe                                   # 测试
build/kc_tests.exe --speed <源目录> <目标目录> 5000 4 512   # 速度：5000 个小文件 + 4 个 512 MB
cmake -S . -B build-release -G Ninja -DKAIROCOPY_STATIC=ON && cmake --build build-release   # 发布：只依赖系统 DLL
bash tools/package.sh v0.1.0
```

第三方：SQLite（公有领域）、xxHash（BSD-2）。
