#ifndef _CONFIGSTORE_H
#define _CONFIGSTORE_H

#include <stdint.h>

struct patcher_config
{
    //const char* version;
    uint32_t sp;    // ��λ�������к͸���λ��
    uint32_t lr;    // ����dispatchcmd����ִ��
    uint32_t usbrx;
    bool isSES;     // SES��Ҫʹ��R4-R6ֵ
    char cmdReg;    // ������ʹ�õļĴ���
    uint32_t R4, R5, R6;
    bool nopad;     // ջĩβû��4�ֽڿն�
};

tm get_build_date(const char* version);
const patcher_config* find_patcher_config(const char* fwversion);
const patcher_config* analyst_firmware_stack(const void* fwbuf, size_t fwlen);
bool add_user_config(const char* fwversion, const patcher_config* config);

#endif