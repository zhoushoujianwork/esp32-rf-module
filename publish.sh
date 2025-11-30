#!/bin/bash

# ESP32 RF Module 发布脚本
# 用法: ./publish.sh [版本号]
# 例如: ./publish.sh 0.1.5
# 
# 需要设置环境变量:
#   export IDF_COMPONENT_API_TOKEN=your_token_here

set -e  # 遇到错误立即退出

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# 检查是否在正确的目录
if [ ! -f "idf_component.yml" ]; then
    echo -e "${RED}错误: 请在组件根目录运行此脚本${NC}"
    exit 1
fi

# 检查 token
if [ -z "$IDF_COMPONENT_API_TOKEN" ]; then
    echo -e "${RED}错误: 请设置环境变量 IDF_COMPONENT_API_TOKEN${NC}"
    echo "例如: export IDF_COMPONENT_API_TOKEN=your_token_here"
    exit 1
fi

# 获取版本号
if [ -z "$1" ]; then
    echo -e "${YELLOW}用法: $0 [版本号]${NC}"
    echo "例如: $0 0.1.5"
    exit 1
fi

VERSION=$1
TAG="v${VERSION}"

# 验证版本号格式 (简单验证: 数字.数字.数字)
if ! [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo -e "${RED}错误: 版本号格式不正确，应为 x.y.z (例如 0.1.5)${NC}"
    exit 1
fi

echo -e "${GREEN}开始发布版本 ${VERSION}...${NC}"

# 1. 更新 idf_component.yml 中的版本号
echo -e "${YELLOW}[1/6] 更新版本号...${NC}"
if [[ "$OSTYPE" == "darwin"* ]]; then
    # macOS
    sed -i '' "s/version: \".*\"/version: \"${VERSION}\"/" idf_component.yml
else
    # Linux
    sed -i "s/version: \".*\"/version: \"${VERSION}\"/" idf_component.yml
fi

# 验证更新
CURRENT_VERSION=$(grep "^version:" idf_component.yml | sed 's/version: "\(.*\)"/\1/')
if [ "$CURRENT_VERSION" != "$VERSION" ]; then
    echo -e "${RED}错误: 版本号更新失败${NC}"
    exit 1
fi
echo -e "${GREEN}✓ 版本号已更新为 ${VERSION}${NC}"

# 2. 更新 README.md 中的版本号（如果存在）
echo -e "${YELLOW}[2/6] 更新 README.md...${NC}"
if [ -f "README.md" ]; then
    # 更新更新日志中的版本号（查找 ### v 开头的行）
    if [[ "$OSTYPE" == "darwin"* ]]; then
        # macOS
        sed -i '' "s/^### v[0-9]\+\.[0-9]\+\.[0-9]\+/### v${VERSION}/" README.md || true
    else
        # Linux
        sed -i "s/^### v[0-9]\+\.[0-9]\+\.[0-9]\+/### v${VERSION}/" README.md || true
    fi
    echo -e "${GREEN}✓ README.md 已更新${NC}"
fi

# 3. 检查 Git 状态
echo -e "${YELLOW}[3/6] 检查 Git 状态...${NC}"
if [ -n "$(git status --porcelain)" ]; then
    echo -e "${GREEN}✓ 检测到未提交的更改${NC}"
else
    echo -e "${YELLOW}警告: 没有检测到更改，继续执行...${NC}"
fi

# 4. 提交更改
echo -e "${YELLOW}[4/6] 提交更改到 Git...${NC}"
git add idf_component.yml README.md 2>/dev/null || true
git commit -m "Bump version to ${VERSION}" || {
    echo -e "${YELLOW}警告: 提交失败或没有更改需要提交${NC}"
}
echo -e "${GREEN}✓ 更改已提交${NC}"

# 5. 创建并推送 tag
echo -e "${YELLOW}[5/6] 创建并推送 Git tag...${NC}"
# 删除本地 tag（如果存在）
git tag -d "${TAG}" 2>/dev/null || true
# 创建新 tag
git tag -a "${TAG}" -m "Version ${VERSION}"
# 推送代码
git push origin main || {
    echo -e "${RED}错误: 推送代码失败${NC}"
    exit 1
}
# 推送 tag
git push origin "${TAG}" || {
    echo -e "${RED}错误: 推送 tag 失败${NC}"
    exit 1
}
echo -e "${GREEN}✓ Tag ${TAG} 已创建并推送${NC}"

# 6. 上传到 ESP-IDF 组件注册表
echo -e "${YELLOW}[6/6] 上传到 ESP-IDF 组件注册表...${NC}"

# 检查 Python 和 idf_component_manager
PYTHON_CMD=""
if command -v python3.11 &> /dev/null; then
    PYTHON_CMD="python3.11"
elif command -v python3 &> /dev/null; then
    PYTHON_CMD="python3"
else
    echo -e "${RED}错误: 未找到 python3${NC}"
    exit 1
fi

# 检查 idf_component_manager 是否安装
if ! $PYTHON_CMD -m idf_component_manager version &> /dev/null; then
    echo -e "${YELLOW}正在安装 idf-component-manager...${NC}"
    $PYTHON_CMD -m pip install idf-component-manager --quiet
fi

# 上传组件
export IDF_COMPONENT_API_TOKEN
$PYTHON_CMD -m idf_component_manager component upload \
    --name esp32-rf-module \
    --version "${VERSION}" \
    --namespace zhoushoujianwork || {
    echo -e "${RED}错误: 上传到组件注册表失败${NC}"
    exit 1
}

echo ""
echo -e "${GREEN}════════════════════════════════════════${NC}"
echo -e "${GREEN}✓ 版本 ${VERSION} 发布成功！${NC}"
echo -e "${GREEN}════════════════════════════════════════${NC}"
echo ""
echo "组件信息:"
echo "  - 名称: zhoushoujianwork/esp32-rf-module"
echo "  - 版本: ${VERSION}"
echo "  - Tag: ${TAG}"
echo ""
echo "查看地址:"
echo "  https://components.espressif.com/components/zhoushoujianwork/esp32-rf-module/versions/${VERSION}"
echo ""
echo -e "${YELLOW}注意: 新版本可能需要最多 5 分钟才能在全球范围内可用${NC}"

