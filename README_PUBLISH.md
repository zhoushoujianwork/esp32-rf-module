# 发布脚本使用说明

## 快速开始

### 1. 设置环境变量

```bash
export IDF_COMPONENT_API_TOKEN=your_token_here
```

### 2. 运行发布脚本

```bash
./publish.sh 0.1.5
```

## 脚本功能

发布脚本 `publish.sh` 会自动完成以下步骤：

1. ✅ **更新版本号** - 自动更新 `idf_component.yml` 和 `README.md` 中的版本号
2. ✅ **提交更改** - 自动提交版本更新到 Git
3. ✅ **创建 Tag** - 创建并推送 Git tag (例如 `v0.1.5`)
4. ✅ **推送到 GitHub** - 推送代码和 tag 到远程仓库
5. ✅ **上传到组件注册表** - 自动上传到 ESP-IDF 组件注册表

## 使用示例

```bash
# 设置 token
export IDF_COMPONENT_API_TOKEN=v1wVMcHU0T7JRSONPdz1L1L16Ho-NqHVFPChmj9X1iS9-ZNmFhOd5yRfZyy7Ulgx6Anljl13mNaQDIbmhmHoyQ

# 发布新版本
./publish.sh 0.1.5
```

## 版本号格式

版本号必须遵循语义化版本格式：`x.y.z`

- `x` - 主版本号（重大变更）
- `y` - 次版本号（新功能）
- `z` - 修订版本号（bug 修复）

示例：
- ✅ `0.1.5` - 正确
- ✅ `1.0.0` - 正确
- ❌ `0.1` - 错误（缺少修订版本号）
- ❌ `v0.1.5` - 错误（不要包含 'v' 前缀）

## 注意事项

1. **Token 安全**：不要将 token 提交到 Git 仓库
2. **版本号**：确保版本号是递增的
3. **Git 状态**：确保工作目录干净，或至少没有重要的未提交更改
4. **网络连接**：上传到组件注册表需要网络连接

## 故障排除

### Token 未设置
```
错误: 请设置环境变量 IDF_COMPONENT_API_TOKEN
```
**解决方案**：运行 `export IDF_COMPONENT_API_TOKEN=your_token`

### 版本号格式错误
```
错误: 版本号格式不正确，应为 x.y.z
```
**解决方案**：使用正确的格式，例如 `0.1.5`

### Python 未找到
```
错误: 未找到 python3
```
**解决方案**：确保已安装 Python 3.11 或更高版本

### 上传失败
如果上传失败，可以手动检查：
1. Token 是否有效
2. 网络连接是否正常
3. 组件名称和命名空间是否正确

## Windows 用户

Windows 用户可以使用 `publish.bat` 脚本：

```cmd
set IDF_COMPONENT_API_TOKEN=your_token_here
publish.bat 0.1.5
```

