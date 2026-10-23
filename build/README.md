# 编译与签名

## 1. 编译

在仓库根目录执行（产物统一进 `bin\`）：

```bat
make.bat
:: 或
build\build_bypass.bat
build\build_hook32.bat
```

## 2. 签名

### 2.1 生成内网测试用自签证书

```bat
powershell -ExecutionPolicy Bypass -File build\make-selfsigned-cert.ps1 -Trust
```

- 证书和 PFX 输出到 `local\certs\`（`.gitignore` 已排除，不会入库）。
- `-Trust` 会把证书装进当前用户的 `TrustedPublisher` 和 `Root`，仅对本机有效。
- 不要把 PFX / 私钥提交到仓库。

### 2.2 签名 `bin\` 下的三个交付文件

```bat
build\sign.bat local\certs\bypass_tool-test.pfx
```

- 会提示输入 PFX 密码；密码不会出现在命令行里。
- 离线环境加第二个参数 `notimestamp`：

```bat
build\sign.bat local\certs\bypass_tool-test.pfx notimestamp
```

等价的 PowerShell 写法（适合自动化，注意密码会出现在命令行）：

```powershell
powershell -ExecutionPolicy Bypass -File build\sign.ps1 `
  -Pfx local\certs\bypass_tool-test.pfx -Password "your-password" -NoTimestamp
```

### 2.3 目标机信任自签证书

把 `local\certs\bypass_tool-test.cer` 导入目标机的：

- `Cert:\CurrentUser\TrustedPublisher`
- `Cert:\CurrentUser\Root`

或在目标机执行：

```powershell
Import-Certificate -FilePath bypass_tool-test.cer -CertStoreLocation Cert:\CurrentUser\TrustedPublisher
Import-Certificate -FilePath bypass_tool-test.cer -CertStoreLocation Cert:\CurrentUser\Root
```

未导入时，`Get-AuthenticodeSignature` 会显示 `UnknownError`，但文件本身已经带签名。

## 3. 正式分发

- 建议使用 OV/EV 代码签名证书或 Azure Trusted Signing。
- 重新编译后需要重新签名；签名后文件大小和 MD5 会变。
- 自签名只能改善本机/内网显示，**不能保证**不被 `Trojan:Win32/Wacatac.B!ml` 这类 ML 误报命中。
- PFX / 私钥只放本机或 CI Secret，绝不提交仓库。

## 4. 打包 Release

先确保 `bin\` 已签名，然后：

```bat
powershell -ExecutionPolicy Bypass -File build\package.ps1 -Version v1.2.0 -IncludeCert
```

- 产物：`dist\classroom-window-helper-v1.2.0.zip`
- 包含：三个交付文件、`bypass_config.ini`、`README.md`、`docs\how-to-use.md`、`docs\design.md`；加 `-IncludeCert` 还会带 `bypass_tool-test.cer` 和信任说明。
- 不包含：`local\`、PFX、日志、netlab。
- `dist\` 已在 `.gitignore` 中，zip 不提交，直接上传到 GitHub **Releases** 附件。

如果编译后还没签名，可以让 `package.ps1` 先签：

```powershell
.\build\package.ps1 -Version v1.2.0 -IncludeCert `
  -Pfx local\certs\bypass_tool-test.pfx -Password "your-password"
```
