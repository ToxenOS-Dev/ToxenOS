#include "../tox.h"
void _start() {
    char args[256]; get_args(args);
    if(!args[0]){print("Usage: file <name>\n");tox_exit();}
    if(tox_stat(args)<0){set_color(0x0C);print("file: not found\n");set_color(0x07);tox_exit();}
    file_type_t ft=get_file_type(args);
    set_color(type_color(ft));print(args);set_color(0x07);print(": ");
    switch(ft){
        case FTYPE_TEXT:    print("text file\n"); break;
        case FTYPE_SCRIPT:  print("ToxenOS script\n"); break;
        case FTYPE_IMAGE:   print("image file\n"); break;
        case FTYPE_AUDIO:   print("audio file\n"); break;
        case FTYPE_VIDEO:   print("video file\n"); break;
        case FTYPE_ARCHIVE: print("archive\n"); break;
        case FTYPE_BINARY:  print("executable\n"); break;
        default:            print("unknown type\n"); break;
    }
    tox_exit();
}
