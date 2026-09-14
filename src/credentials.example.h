#ifndef CREDENTIALS_H
#define CREDENTIALS_H

/*
 * 郑大校园网认证凭据配置
 *
 * 使用方法：
 *   1. 复制本文件为 credentials.h：
 *      cp src/credentials.example.h src/credentials.h
 *   2. 编辑 src/credentials.h，填入你的学号、密码和运营商后缀
 *   3. 重新编译：make
 *
 * 【重要安全提示】
 *   src/credentials.h 已在 .gitignore 中忽略，
 *   切勿将包含真实密码的 credentials.h 强制提交到公开仓库！
 */

/* 校园网账号（学号） */
#define AUTH_USER     "your_username_here"

/* 校园网密码 */
#define AUTH_PASSWORD "your_password_here"

/* 运营商后缀：
 *   - 校园网原生 (教育网/内网): ""
 *   - 移动融合宽带:            "@cmcc"
 *   - 联通融合宽带:            "@unicom"
 *   - 电信融合宽带:            "@telecom"
 */
#define AUTH_SUFFIX   ""

#endif /* CREDENTIALS_H */

