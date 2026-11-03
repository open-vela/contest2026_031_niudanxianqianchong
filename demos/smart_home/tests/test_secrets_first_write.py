#!/usr/bin/env python3
"""Check first save without an existing secrets directory."""
from pathlib import Path
import subprocess,tempfile
repo=Path(__file__).resolve().parents[3]; root=repo.parent if (repo.parent/'apps').is_dir() else repo.parent.parent; src=repo/'demos/smart_home/src/config'; cjson=root/'apps/netutils/cjson/cJSON'
with tempfile.TemporaryDirectory(prefix='secrets-first-') as tmp:
    tmp=Path(tmp); (tmp/'nuttx').mkdir(); (tmp/'nuttx/config.h').write_text('#define FAR\n')
    main=tmp/'main.c'; main.write_text('''#include "smart_home_secrets.h"\n#include <assert.h>\n#include <string.h>\nint main(void) { char s[33],p[65],h[64],t[64]; unsigned short port; assert(smart_home_secrets_set_wifi_credentials("ssid","pw")==0); assert(smart_home_secrets_set_miloco("host",1810,"token")==0); assert(smart_home_secrets_get_wifi_credentials(s,sizeof(s),p,sizeof(p))==0); assert(!strcmp(s,"ssid")&&!strcmp(p,"pw")); assert(smart_home_secrets_get_miloco(h,sizeof(h),&port,t,sizeof(t))==0); assert(!strcmp(h,"host")&&port==1810&&!strcmp(t,"token")); return 0; }\n''')
    secret=tmp/'fresh'/'secrets.json'; exe=tmp/'test'
    command=['cc','-std=c11','-D_POSIX_C_SOURCE=200809L','-DFAR=','-Wall','-Wextra','-Werror',f'-DCONFIG_SMART_HOME_MODEL_SECRETS_PATH="{secret}"','-I',str(tmp),'-I',str(src),'-I',str(cjson),'-I',str(repo/'packages/cAGENT/include'),str(main),str(src/'smart_home_secrets.c'),str(cjson/'cJSON.c'),'-pthread','-lm','-o',str(exe)]
    subprocess.run(command,check=True); subprocess.run([str(exe)],check=True)
    print('PASS: create missing secrets directory and preserve Wi-Fi/Miloco credentials')
