J-Link EDU Reviver
==================

ʹ�÷���
--------
��ѹ�����ļ���ͬһĿ¼��, ����������ִ��

EDUReViver -run blinky  
���Ʋ���, ��������˸12��, ˵���豸��ͨ��У��������ͨѶ����.  

EDUReViver -run revive  
ȥ�������FlashDL֮���Features.  

EDUReViver -run swd  
��SWD���Խӿ�.  

EDUReViver -run to11  
ת��V10��V11. ���²�κ�, ��jlink commander���Զ�ˢ�¹̼�.  

EDUReViver -run to10  
ת��V11��V10. ���²�κ�, ��jlink commander���Զ�ˢ�¹̼�.  

֧���豸
--------
J-Link V10/V11 ���а汾, ĿǰΪֹ���й̼�. ֧�ִ��������Features������. ��������ɽկ��. �Ҽ���һЩ���߼���������.
һ��ֻ��֧��һ���豸, ֧������ΪWinUSB����, �������ʶ�����, ����JLinkConfig���ó������л���Segger����.

��������
--------
�˹��߽����о���ѧϰʹ��, ������������ҵ��;. ������ɽկ����ʹ��, �п��ܵ��¹����¼�.


Introduce
---------
This utility support all version of JLinkV10/V11. support genius brick which added bad features.
don't support illegal clones. some check applied on my utility to prevent running on clones.
turn off winusb mode in JlinkConfig if you meet usb communicate problems.

Usage
--------
extract all files to same folder.
execute these commands in command prompt.

EDUReViver -run blinky  
this payload blinks red LED 12 times. this should used to test your usb and network connection is fine.  
  
EDUReViver -run revive  
this payload remove all extra features.  

EDUReViver -run swd  
this payload unlocking SWD debug port on PCB.  

EDUReViver -run to11  
this payload turns your V10 to V11. unplug and replug usb cable, then flash new firmware in jlink commander.  

EDUReViver -run to10  
this payload turns your V11 to V10. unplug and replug usb cable, then flash new firmware in jlink commander.  

Disclaimer
----------
You can only use my utility for research propose, or repair your genius jlink.