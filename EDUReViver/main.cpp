#include "targetver.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <conio.h>
#include <stdint.h>
#include <setupapi.h>
#include <time.h>
#include <WinCrypt.h>
//#include <intrin.h>
#include <io.h>
#include "addon_func.h"
#include "configstore.h"
#include "crypto.h"
#include "usbconn.h"
#include "httpclient.h"


uint32_t encode_fwLen(uint32_t fwlen);
uint16_t crc16_kermit(const uint8_t *buf, size_t len);
uint32_t calc_sn_checksum(uint32_t sn);
uint32_t crc32_rev(uint8_t *buf, size_t len);
// check
bool is_BTL_version(const char* fwversion);
bool is_offical_bootloader(const void* btl);

void printdigest(const char* name, const uint8_t* digest, size_t len);
char* dump2hex(const uint8_t* buf, size_t len, char* hex);
char* base64_encode(const uint8_t* value, size_t valuelen);
void printfeatures(const uint8_t* flash);
void printCRPlevel(uint32_t crp);

#pragma pack(push, 1)
struct cmd_fine_write_read
{
    uint8_t cmd; // E0
    uint32_t writelen;      // sp+8 max 0x10
    uint32_t readlen;       // sp+c max 0x10
    uint32_t somelen;       // sp+10
    uint8_t writebuf[0x10]; // sp+14
    uint8_t remotebuf[0x14];// sp+24 (0x14+4 extra byte)
};
struct cmd_fine_write_read_IAR : cmd_fine_write_read {
    uint32_t pad;           // sp+38
    uint32_t regLR;         // sp+3C = LR
};
struct cmd_fine_write_read_SES : cmd_fine_write_read {
    uint32_t readed;        // sp+34
    uint32_t R4;            // sp+38
    uint32_t R5;            // sp+3C
    uint32_t R6;            // sp+40
    uint32_t regLR;         // sp+44 = LR
};
struct cmd_fine_write_read_SES_new : cmd_fine_write_read {
    uint32_t R4;            // sp+30
    uint32_t R5;            // sp+34
    uint32_t R6;            // sp+38
    uint32_t regLR;         // sp+3C = LR
};
#pragma pack(pop)

cmd_fine_write_read* assembly_cmd_payload(int* cmdlen, const void* payload, size_t payloadlen, const patcher_config* config, size_t readlen);

cmd_fine_write_read* assembly_cmd_payload(int* cmdlen, const void* payload, size_t payloadlen, const patcher_config* config, size_t readlen)
{
    size_t newlen = config->isSES?(config->nopad?sizeof(cmd_fine_write_read_SES_new):sizeof(cmd_fine_write_read_SES)):sizeof(cmd_fine_write_read_IAR); // 38+1, 44+1
    size_t payloadoffset = (uint32_t)&((cmd_fine_write_read*)0)->somelen;
    if (payloadoffset + payloadlen > newlen) {
        newlen = payloadoffset + payloadlen; // overlay
    }
    if (cmdlen) {
        *cmdlen = newlen;
    }
    cmd_fine_write_read* cmd = (cmd_fine_write_read*)malloc(newlen);
    cmd->cmd = 0xE0;
    cmd->writelen = newlen - 1 - 0xC; // 2C/38/34
    cmd->readlen = readlen;
    memcpy((char*)cmd + payloadoffset, payload, payloadlen); // 发送后40B0处是我们代码
    if (config->isSES) {
        if (config->nopad) {
            cmd_fine_write_read_SES_new* ses = (cmd_fine_write_read_SES_new*)cmd;
            ses->regLR = config->sp + 0x8 | 1; // BLX rxbuf时SP
            ses->R4 = config->R4;
            ses->R5 = config->R5;
            ses->R6 = config->R6;
        } else {
            cmd_fine_write_read_SES* ses = (cmd_fine_write_read_SES*)cmd;
            ses->regLR = config->sp + 0xC | 1;
            ses->R4 = config->R4;
            ses->R5 = config->R5;
            ses->R6 = config->R6;
        }
    } else {
        cmd_fine_write_read_IAR* iar = (cmd_fine_write_read_IAR*)cmd;
        iar->regLR = config->sp + 0x10 | 1; // sp是BL rxbuf时候值. +4&readed,+8&arg,+10指向&somelen(payload入口点)
    }
    return cmd;
}

enum payloadmode {
    pmM4Ret,    // usbrx
    pmM4Reset,  // m0patcher + usbrx
    pmM0Hold,   // m0patcher + m0reset,usbrx,m0reset
    pmM0Boot    // m0patcher + uxbrx,m0boot
};


#define CLOSE_AND_EXIT(code) freeJLinks(devvec);\
    UnloadWinusb();\
    return code;

void printmismatch(const char* name, bool mismatch)
{
    if (mismatch) {
        errprintf("%s mismatched!\n", name);
    } else {
        printf("%s OK.\n", name);
    }
}

int main(int argc, char * argv[])
{
//#ifdef _DEBUG
//    if (IsDebuggerPresent()) {
//        STARTUPINFOA si = {sizeof(si)}; PROCESS_INFORMATION pi = {};
//        if (CreateProcessA(NULL,
//            "\"F:\\Program Files\\SystemTools\\ConEmu5\\ConEmu\\ConEmuC.exe\" /AUTOATTACH",
//            NULL, NULL, FALSE, NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi))
//        { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
//    }
//#endif
    int mode = -1;
    char* payloadname = 0;
    char* payloadopt = 0;
    if (argc >= 3) {
        for (int i = 1; i < argc; i++) {
            if (_stricmp(argv[i], "-run") == 0) {
                payloadname = argv[++i];
                continue;
            }
            if (payloadopt) {
                payloadopt = (char*)realloc(payloadopt, strlen(payloadopt) + strlen(argv[i]) + 2);
                char* tail = payloadopt + strlen(payloadopt);
                *tail++ = ' ';
                strcpy_s(tail, strlen(payloadopt) + 1, argv[i]);
            } else {
                payloadopt = _strdup(argv[i]);
            }
        }
    }
    if (payloadname == 0) {
        printf("V10ReViver -run <blinky|revive|swd|to11|to10> [options]\n");
        return -1;
    }

    if (LoadWinusb() != 0) {
        printf("No WinUSB support because WinUSB.dll is corrupt or missing.\n");;
    }
    JLinkDevVec devvec;
    if (!getJLinks(devvec)) {
        errprintf("Failed to find device!\n");
        CLOSE_AND_EXIT(0);
    } else if (devvec.size() > 1) {
        errprintf("Only support one device, please unplug other probes!\n");
        CLOSE_AND_EXIT(0);
    }

    uint8_t dataBuffer[512] = {0};
    bool isv10 = false;
    jlinkLoopReadFirmwareVersion(&devvec[0], dataBuffer);
    dataBuffer[0x70] = 0;
    //isv9 = strstr((const char*)dataBuffer, "V9") != 0;
    isv10 = (strstr((const char*)dataBuffer, "V10") != 0) || (strstr((const char*)dataBuffer, "V11") != 0) || (strstr((const char*)dataBuffer, "V12") != 0);
    if (is_BTL_version((char*)dataBuffer)) {
        errprintf("Please quit Bootloader mode.\n");
        CLOSE_AND_EXIT(0);
    } else {
        printf("Firmware Version: %s\n", dataBuffer);
    }
    if (isv10 == false) {
        errprintf("Only support v10,v11,v12 devices.\n");
        CLOSE_AND_EXIT(0);
    }
    // check genius hardware
    const patcher_config* config = find_patcher_config((char*)dataBuffer);
    uint32_t sn, snchecksum;
    uint8_t snuidbuf[36];
    void* otssign;
    bool sncheckerror = true;
    bool touchfeatures = _stricmp(payloadname, "revive") == 0;
    bool touchsn = _stricmp(payloadname, "setsn") == 0;
    bool touchcrp = _stricmp(payloadname, "swd") == 0;
    if (jlinkCommandReadOTS(&devvec[0], dataBuffer)) {
        sn = *(uint32_t*)dataBuffer;
        snchecksum = *(uint32_t*)(dataBuffer + 4);
        otssign = dataBuffer + 0x100;
        *(uint32_t*)snuidbuf = sn;
        sncheckerror = calc_sn_checksum(sn) != snchecksum;
        printf("sn: %d, ", sn);
        printmismatch("checksum", sncheckerror);
        if (touchfeatures) {
            printfeatures(&dataBuffer[0x20]);
        }
    }
    uint32_t uidlen = 32;
    bool otssignok = false;
    void* myapp = 0;
    int applen = 0;
    if (jlinkCommandReadUID(&devvec[0], &uidlen, &snuidbuf[4])) {
        char* uidstr = base64_encode(&snuidbuf[4], uidlen);
        char* signstr = base64_encode((uint8_t*)otssign, 0x100);
        char* reply = 0;
        size_t replylen = 0;
        int reqret = request_payload_online(sn, uidstr, signstr, payloadname, payloadopt, &reply, &replylen);
        if (payloadopt) {
            free(payloadopt);
        }
        if (uidstr) {
            free(uidstr);
        }
        if (signstr) {
            free(signstr);
        }
        if (reqret == 0 && replylen >= 8) {
            int otsmatched = *(int32_t*)reply;
            mode = *(int32_t*)(reply + 4);
            applen = replylen - 8;
            if (applen) {
                myapp = malloc(applen);
                memcpy(myapp, reply + 8, applen);
            }
            otssignok = otsmatched == 1;
        } else {
            errprintf("Failed to retrieve payload due to a network error %d!\n", reqret);
            CLOSE_AND_EXIT(0);
        }
        if (reply) {
            free(reply);
        }
        printmismatch("UID signature", !otssignok);
    } else {
        if (payloadopt) {
            free(payloadopt);
        }
        errprintf("Reading UID failed!\n");
        CLOSE_AND_EXIT(0);
    }
    uint32_t dumpsize = config?0x8000:0x80000;
    // dump bootloader/firmware and parse
    char* fwdump = (char*)malloc(dumpsize);
    if (jlinkDumpFullFirmware(&devvec[0], 0x1A000000, dumpsize, fwdump)) {
        bool bootloaderok = is_offical_bootloader(fwdump);
        if (touchcrp) {
            printCRPlevel(*(uint32_t*)&fwdump[0x2FC]);
        }
        if (sn == -1 || sncheckerror || bootloaderok == false || otssignok == false) {
            errprintf("Detected clone.\n");
#ifdef DENYCLONE
            free(fwdump);
            if (myapp) {
                free(myapp);
            }
            CLOSE_AND_EXIT(0);
#endif
        }
    } else {
        free(fwdump);
        errprintf("Dumping failed. Can't check device's genius.\n");
        if (myapp) {
            free(myapp);
        }
        CLOSE_AND_EXIT(0);
    }
    if (myapp == 0) {
        errprintf("Payload \"%s\" was not found on server!\n", payloadname);
        free(fwdump);
        CLOSE_AND_EXIT(0);
    }

    if (config == 0) {
        printf("Your version number is not present in config cache! Analyst it now...\n");
        config = analyst_firmware_stack(fwdump + 0x8000, 0x78000);
    }
    free(fwdump);
    if (config == NULL) {
        free(myapp);
        CLOSE_AND_EXIT(0);
    }

    uint32_t oldif;
    if (jlinkCommandSendSelectInterface(&devvec[0], 3, &oldif)) {
        printf("Change interface: %d -> 3\n", oldif);
    } else {
        errprintf("Select interface failed!\n");
        free(myapp);
        CLOSE_AND_EXIT(0);
    }
    // 前置步骤: 准备超长payload的执行环境
    bool M0patched = false;
    int cmdlen;
    uint32_t readed; // FINE采样字节数, m0app设置
    // remotebuf 前4字节在设备会被填充readed, 因此不能放代码, 4+writebuf和remotebuf-4各可以放0x14大小代码
    // 此处代码要注意执行时候sp是100840E0(是LR末尾), 如有push会首先破坏LR位置,再继续往前破坏可能破坏自身
    // SES的代码局部变量readed会额外填充+28空隙, 导致第二段要拆分出来一个literal放入R4-R6区域
    if (mode == pmM4Ret && (config->isSES == false || (config->cmdReg >= 4 && config->cmdReg <= 6))) {
        // 单次溢出
        cmd_fine_write_read* m4rxcmd;
        if (config->isSES) {
            // SES版+2C是R4位置, SES新版+28就是R4位置, 此代码有效长度2C(28+4)
            unsigned char m4rxret[0x30] = {
                0x8C, 0xB0, 0x0A, 0x48, 0x4F, 0xF4, 0x00, 0x61, 0x05, 0x4A, 0x90, 0x47, 0x07, 0x48, 0x01, 0x30,
                0x80, 0x47, 0x01, 0xE0, 0xFF, 0xFF, 0xFF, 0xFF, 0x0C, 0xB0, 0xDF, 0xF8, 0x08, 0xF0, 0x00, 0x00,
                0xDB, 0x0E, 0x01, 0x1A, 0x19, 0x07, 0x01, 0x1A, 0xFF, 0xFF, 0xFF, 0xFF, 0x50, 0x00, 0x00, 0x20
            };
            *(uint32_t*)&m4rxret[0x20] = config->usbrx | 1;
            *(uint32_t*)&m4rxret[0x24] = config->lr | 1; // 要返回dispatchcmd
            // 孤单literal的重定位
            m4rxret[0x2] += config->cmdReg - (config->nopad?5:4);
            m4rxret[0xC] += config->cmdReg - (config->nopad?5:4);
            m4rxcmd = assembly_cmd_payload(&cmdlen, m4rxret, sizeof(m4rxret)-8, config, 0);
            if (config->nopad) {
                *(&((cmd_fine_write_read_SES_new*)m4rxcmd)->R4 + config->cmdReg - 4) = *(uint32_t*)&m4rxret[0x2C]; // literal
            } else {
                *(&((cmd_fine_write_read_SES*)m4rxcmd)->R4 + config->cmdReg - 4) = *(uint32_t*)&m4rxret[0x2C]; // literal
            }
        } else {
            unsigned char m4rxret[0x2C] = {
                0x8C, 0xB0, 0x07, 0x48, 0x4F, 0xF4, 0x00, 0x61, 0x06, 0x4A, 0x90, 0x47, 0x04, 0x48, 0x01, 0x30,
                0x80, 0x47, 0x01, 0xE0, 0xFF, 0xFF, 0xFF, 0xFF, 0x0C, 0xB0, 0xDF, 0xF8, 0x0C, 0xF0, 0x00, 0x00,
                0x50, 0x00, 0x00, 0x20, 0xDB, 0x0E, 0x01, 0x1A, 0x19, 0x07, 0x01, 0x1A 
            };
            *(uint32_t*)&m4rxret[0x24] = config->usbrx | 1;
            *(uint32_t*)&m4rxret[0x28] = config->lr | 1; // 要返回dispatchcmd
            m4rxcmd = assembly_cmd_payload(&cmdlen, m4rxret, sizeof(m4rxret), config, 0);
        }
        jlinkSendCommand(&devvec[0], m4rxcmd, cmdlen, &readed, sizeof(readed));
        free(m4rxcmd);
    } else {
        // 双次溢出
        // Patcher执行时候修补M0代码, 此刻M0正在10000068之间不断的循环, 因此我们补循环之外的东西不需要重启M0
        // 因为Patcher<=28, 所以兼容IAR和SES固件
        unsigned char patcher[0x2C] = {
            0x06, 0x49, 0x44, 0xF2, 0x6D, 0x00, 0x08, 0x80, 0x04, 0x49, 0xE2, 0x31, 0x43, 0xF6, 0x03, 0x00,
            0x08, 0x80, 0x01, 0xE0, 0xFF, 0xFF, 0xFF, 0xFF, 0xDF, 0xF8, 0x04, 0xF0, 0x72, 0x00, 0x00, 0x10,
            0x19, 0x07, 0x01, 0x1A 
        };
        *(uint32_t*)&patcher[0x20] = config->lr + 1; // 第一次要返回dispatchcmd
        cmd_fine_write_read* patchercmd = assembly_cmd_payload(&cmdlen, patcher, sizeof(patcher), config, 0);
        jlinkSendCommand(&devvec[0], patchercmd, cmdlen, &readed, sizeof(readed));
        free(patchercmd);
        M0patched = true;
        // 此时代码可以使用连续的0x2C/0x28内容, 其他注意事项同上
        if (mode == pmM0Hold || mode == pmM0Boot) {
            // 接收器模式
            cmd_fine_write_read* ldrcmd;
            if (mode == pmM0Hold) {
                // usbresetrxm0: usbrx
                unsigned char m0ldr[0x2C] = {
                    0x8C, 0xB0, 0x07, 0x49, 0x5F, 0xF0, 0x80, 0x70, 0x08, 0x60, 0x81, 0x0B, 0x00, 0x01, 0x05, 0x4A,
                    0x90, 0x47, 0x03, 0x49, 0x00, 0x20, 0x08, 0x60, 0x0C, 0xB0, 0xDF, 0xF8, 0x0C, 0xF0, 0x00, 0x00,
                    0x04, 0x31, 0x05, 0x40, 0xDB, 0x0E, 0x01, 0x1A, 0x19, 0x07, 0x01, 0x1A 
                };
                *(uint32_t*)&m0ldr[0x24] = config->usbrx | 1;
                *(uint32_t*)&m0ldr[0x28] = config->lr | 1;
                ldrcmd = assembly_cmd_payload(&cmdlen, m0ldr, sizeof(m0ldr), config, -0x18);
            } else {
                // usbrxbootm0: usbrx bootm0 dispatchlr
                unsigned char m0ldr[0x2C] = {
                    0x8C, 0xB0, 0x05, 0x48, 0x05, 0x49, 0x06, 0x4A, 0x90, 0x47, 0x03, 0x48, 0x03, 0x49, 0x05, 0x4A,
                    0x90, 0x47, 0x0C, 0xB0, 0xDF, 0xF8, 0x10, 0xF0, 0x48, 0x00, 0x00, 0x20, 0x00, 0x08, 0x00, 0x00,
                    0xDB, 0x0E, 0x01, 0x1A, 0x9D, 0x8C, 0x01, 0x1A, 0x19, 0x07, 0x01, 0x1A 
                };
                *(uint32_t*)&m0ldr[0x20] = config->usbrx | 1;
                //*(uint32_t*)&m0ldr[0x24] = configex->bootm0 | 1;
                *(uint32_t*)&m0ldr[0x28] = config->lr | 1;
                ldrcmd = assembly_cmd_payload(&cmdlen, m0ldr, sizeof(m0ldr), config, -0x18);
            }
            jlinkSendCommand(&devvec[0], ldrcmd, cmdlen, NULL, 0);
            free(ldrcmd);
        }
        // SES固件 R4~R6无可用空间情况的第二次溢出, 为了兼容未来固件(但不能不知R4~R6值)
        if (mode == pmM4Ret) {
            unsigned char m4rxret[0x24] = {
                0x8C, 0xB0, 0x05, 0x48, 0x4F, 0xF4, 0x00, 0x61, 0x04, 0x4A, 0x90, 0x47, 0x02, 0x48, 0x01, 0x30,
                0x80, 0x47, 0x0C, 0xB0, 0xDF, 0xF8, 0x08, 0xF0, 0x48, 0x00, 0x00, 0x20, 0xDB, 0x0E, 0x01, 0x1A,
                0x19, 0x07, 0x01, 0x1A 
            };
            *(uint32_t*)&m4rxret[0x1C] = config->usbrx | 1;
            *(uint32_t*)&m4rxret[0x20] = config->lr | 1; // 要返回dispatchcmd
            cmd_fine_write_read* m4rxcmd = assembly_cmd_payload(&cmdlen, m4rxret, sizeof(m4rxret), config, 0);
            jlinkSendCommand(&devvec[0], m4rxcmd, cmdlen, &readed, sizeof(readed));
            free(m4rxcmd);
        }
    }
    // 发送步骤: 发送超长payload
    // M4版运行空间可以是不返回的超写栈, 可以是写20000048或者sub到线程本地栈, M0版是收到sram0带vector
    // M0App它能不能call flash里面的M4函数? 不牵扯到VFP之类的指令, 应该是可以的.
    if (mode == pmM4Reset) {
        // 超写模式, 不需接收器部分, 最后需重启
        // 特殊payload: 不带返回指针, 单2C/28 gap
        cmd_fine_write_read* m4loopcmd = assembly_cmd_payload(&cmdlen, myapp, applen, config, -0x18);
        jlinkSendCommand(&devvec[0], m4loopcmd, cmdlen, NULL, 0);
        free(m4loopcmd);
    } else {
        // 填充到800发出(因为接收器要接满800,可以改loader变为400/200)
        int applenfull = 0x800;
        myapp = realloc(myapp, 0x800);
        memset((char*)myapp + applen, 0, applenfull - applen);
        jlinkSendCommand(&devvec[0], myapp, applenfull, NULL, 0);
    }
    free(myapp);
    // 清理步骤, 如果打过M0补丁, 则恢复补丁, 重启版除外, 等他重启
    if (M0patched && mode != pmM4Reset) {
        uint32_t oldif2;
        if (jlinkCommandSendSelectInterface(&devvec[0], oldif, &oldif2)) {
            printf("Change interface: %d -> %d\n", oldif2, oldif);
        } else {
            errprintf("Select interface failed!\n");
            CLOSE_AND_EXIT(0);
        }
    }
    if (mode == pmM4Reset) {
        freeJLinks(devvec);

        printf("Waiting device re-connect...\n");

        SleepEx(5000, TRUE);
        if (!getJLinks(devvec)) {
            errprintf("Failed to find device!\n");
            return 0;
        } else if (devvec.size() > 1) {
            errprintf("Only support single device, please unplug other probes!\n");
            CLOSE_AND_EXIT(0);
        }
        jlinkLoopReadFirmwareVersion(&devvec[0], dataBuffer);
        if (is_BTL_version((char*)dataBuffer)) {
            printf("Found BTL Mode.\n");
        } else {
            printf("Found Normal Mode.\n");
        }
        quickdump(0, dataBuffer, 0x70);
    }
    if ((touchfeatures || touchsn) && jlinkCommandReadOTS(&devvec[0], dataBuffer)) {
        if (touchsn) {
            sn = *(uint32_t*)dataBuffer;
            snchecksum = *(uint32_t*)(dataBuffer + 4);
            sncheckerror = calc_sn_checksum(sn) != snchecksum;
            printf("new sn: %d, ", sn);
            printmismatch("checksum", sncheckerror);
        }
        if (touchfeatures) {
            printfeatures(&dataBuffer[0x20]);
        }
    }
    if (touchcrp) {
        uint32_t crp;
        if (jlinkDumpFullFirmware(&devvec[0], 0x1A0002FC, 4, &crp)) {
            printCRPlevel(crp);
        }
    }

    freeJLinks(devvec);
    UnloadWinusb();
#ifdef _DEBUG
    _getch();
#endif
    return 0;
}

uint16_t crc16_kermit(const uint8_t *buf, size_t len)
{
    uint16_t crc = 0;
    while (len--) {
        crc ^= *buf++;
        for (int i = 0; i < 8; i++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0x8408;
            } else {
                crc = (crc >> 1);
            }
        }
    }
    return crc;
}

uint32_t encode_fwLen(uint32_t fwlen)
{
    if (fwlen < 0x8000) {
        return fwlen;
    } else {
        // 14bit + 14bit
        return (fwlen & 0x3FFF) | 0xC000 | ((fwlen << 2) & 0x3FFF0000);
    }
}

bool is_BTL_version(const char* fwversion)
{
    //"J-Link V11 compiled Jun  3 2015 BTL     "
    size_t pos = strlen(fwversion);
    if (pos--) {
        while (fwversion[pos] == ' ') {
            pos--;
            if (pos == 0) {
                return false;
            }
        }
        // pos not 0 and point to L
        return strncmp(&fwversion[pos - 2], "BTL", 3) == 0;
    }
    return false;
}

bool sha256(char* buff, size_t buflen, void* digest)
{
    bool ok = false;
    HCRYPTPROV provider;
    HCRYPTHASH hash;
    if (CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
            if (CryptHashData(hash, (BYTE*)buff, buflen, 0)) {
                DWORD digestlen = 32;
                if (CryptGetHashParam(hash, HP_HASHVAL, (BYTE*)digest, &digestlen, 0)) {
                    ok = true;
                }
            }
            CryptDestroyHash(hash);
        }
    }
    CryptReleaseContext(provider, 0);
    return ok;
}

bool aes_256_cbc_encrypt(const uint8_t* key, const uint8_t* iv, uint8_t* buff, size_t buflen)
{
    bool ok = false;
    HCRYPTPROV provider;
    if (CryptAcquireContext(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
        struct AESBLOB256 {
            BLOBHEADER	header;
            DWORD keysize;
            BYTE key[32];
        };
        AESBLOB256 blob;
        blob.header.bType = PLAINTEXTKEYBLOB;
        blob.header.bVersion = CUR_BLOB_VERSION;
        blob.header.reserved = 0;
        blob.header.aiKeyAlg = CALG_AES_256;
        blob.keysize = 32;
        memcpy(blob.key, key, 32);
        HCRYPTKEY cryptkey;
        if (CryptImportKey(provider, (BYTE*)&blob, sizeof(blob), NULL, 0, &cryptkey)) {
            if (CryptSetKeyParam(cryptkey, KP_IV, (const BYTE*)iv, 0)) {
                DWORD mode = CRYPT_MODE_CBC;
                if (CryptSetKeyParam(cryptkey, KP_MODE, (BYTE *)&mode, 0)) {
                    DWORD datlen = buflen;
                    if (CryptEncrypt(cryptkey, NULL, TRUE, 0, NULL, &datlen, 0)){
                        size_t dstlen = datlen; // append 0x10 extra
                        BYTE* dst = (BYTE*)malloc(dstlen);
                        memcpy(dst, buff, buflen);
                        datlen = buflen;
                        if (CryptEncrypt(cryptkey, NULL, TRUE, 0, dst, &datlen, dstlen)){
                            ok = true;
                            memcpy(buff, dst, buflen);
                        }
                        free(dst);
                    }
                }
            }
            CryptDestroyKey(cryptkey);
        }
    }
    CryptReleaseContext(provider, 0);
    return ok;
}

char* base64_encode(const uint8_t* value, size_t valuelen)
{
    if (!value) {
        return 0;
    }
    DWORD required = 0;
    CryptBinaryToStringA(value, valuelen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &required);
    if (required) {
        char* encoded = (char*)malloc(required);
        if (CryptBinaryToStringA(value, valuelen, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, encoded, &required)) {
            return encoded;
        } else {
            free(encoded);
            return 0;
        }
    }
    return 0;
}

bool is_offical_bootloader(const void* btl)
{
    bool matched = false;
    char* buff = (char*)malloc(0x54F8);
    memcpy(buff, btl, 0x54F8);
    *(uint32_t*)&buff[0x2FC] = 0x12345678; // CRP1
    memset(&buff[0x130], 0xFF, 0x70); // Banner
    buff[0x2F89] = '0'; // V10/V11
    unsigned char digest[32];
    if (sha256(buff, 0x54F8, digest)) {
        unsigned char myhash[32] = {
            0x67, 0x90, 0xD1, 0xB9, 0x03, 0x2F, 0x1A, 0x89, 0x0D, 0xE0, 0xB4, 0x56, 0x56, 0x33, 0xE9, 0x79,
            0xC7, 0x82, 0x71, 0x13, 0xB3, 0x28, 0x28, 0x6A, 0xD4, 0xE9, 0xC1, 0x70, 0xE5, 0x3E, 0xB7, 0x47 
        };
        matched = memcmp(digest, myhash, sizeof(myhash)) == 0;
    }
    free(buff);
    return matched;
}

uint32_t calc_sn_checksum(uint32_t sn)
{
    uint8_t iv[16] = {0,};
    uint8_t key[32];
    uint8_t src[256];
    for (int i = 0; i < sizeof(src); i++) {
        src[i] = sn + i;
    }
    for (int i = 0; i < sizeof(key); i++) {
        key[i] = sn >> i;
    }

    if (!aes_256_cbc_encrypt(key, iv, src, sizeof(src))) {
        errprintf("Wincrypt error on your system, can't verify sn checksum!\nPlease make a bugreport on github!\n");
    }

    uint32_t chksum = crc32_rev(src, sizeof(src));
    return chksum;
}

uint32_t crc32_rev(uint8_t *buf, size_t len)
{
    uint32_t crc = 0;
    while (len--) {
        crc ^= *buf++;
        for (int i = 0; i < 8; i++) {
            if (crc & 1) {
                crc = 0xEDB88320 ^ (crc >> 1);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

void printdigest(const char* name, const uint8_t* digest, size_t len)
{
    printf("%s ", name);
    char* line = (char*)_alloca(len * 3);
    for (size_t i = 0, pos = 0; i < len; i++) {
        uint8_t c = digest[i];
        line[pos++] = Nibble2Hex(c >> 4);
        line[pos++] = Nibble2Hex(c & 0xF);
        line[pos++] = ' ';
    }
    fwrite(line, 1, len * 3 - 1, stdout);
    printf("\n");
}

char* dump2hex(const uint8_t* buf, size_t len, char* hex)
{
    for (size_t i = 0, pos = 0; i < len; i++) {
        uint8_t c = buf[i];
        hex[pos++] = Nibble2Hex(c >> 4);
        hex[pos++] = Nibble2Hex(c & 0xF);
    }
    hex[len*2] = 0;
    return hex;
}

void printfeatures(const uint8_t* flash)
{
    printf("Features: ");
    const uint8_t* str = flash;
    bool oldinff = (*flash == 0xFF || *flash == 0);
    flash++;
    bool first = true;
    for (int i = 1; i < 0x80; i++) {
        bool inff = (*flash == 0xFF || *flash == 0);
        if (oldinff == false && inff) {
            if (first == false) {
                printf(", ");
            }
            fwrite(str, flash - str, 1, stdout);
            first = false;
        }
        if (oldinff && inff == false) {
            str = flash;
        }
        oldinff = inff;
        flash++;
    }
    if (oldinff == false) {
        fwrite(str, flash - str, 1, stdout);
    }
    printf("\n");
}

void printCRPlevel(uint32_t crp)
{
    switch (crp)
    {
    case 0xFFFFFFFF:
        printf("NO CRP\n");
        break;
    case 0x12345678:
        printf("CRP1\n");
        break;
    case 0x87654321:
        printf("CRP2\n");
        break;
    case 0x43218765:
        printf("CRP3\n");
        break;
    case 0x4E697370:
        printf("NO ISP\n");
        break;
    default:
        printf("%08X\n", crp);
        break;
    }
}
