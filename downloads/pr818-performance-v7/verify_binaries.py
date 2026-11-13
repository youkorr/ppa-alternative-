#!/usr/bin/env python3
"""Check exported configuration, merged binary hashes and full embedded input pixels."""
import hashlib,json,re,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parent
records=[]
for p in sorted((ROOT/'results').glob('*/*/verification.json')):
 d=json.loads(p.read_text());v=d['variant'];native=v.startswith('native');text=(p.parent/'sdkconfig').read_text()
 config=dict(re.findall(r'^(CONFIG_\w+)=(.+)$',text,re.M))
 numeric={'ESP_DEFAULT_CPU_FREQ_MHZ':360,'SPIRAM_SPEED':200,'CACHE_L2_CACHE_SIZE':131072,'FREERTOS_HZ':1000,'LV_COLOR_DEPTH':16,'LV_DRAW_SW_DRAW_UNIT_CNT':1 if v=='official_ppa1' else 2}
 for key,value in numeric.items():assert int(config['CONFIG_'+key],0)==value,(p,key)
 for key in ('LV_USE_MATRIX','LV_DRAW_TRANSFORM_USE_MATRIX','ESP_LVGL_ADAPTER_ROTATE_IN_DRAW'):
  assert (config.get('CONFIG_'+key)=='y')==native,(p,key)
 assert config.get('CONFIG_SPIRAM_XIP_FROM_PSRAM')!='y'
 firmware=next(p.parent.glob('*.bin'));buf=firmware.read_bytes();assert hashlib.sha256(buf).hexdigest()==d['firmware_sha256']
 w,h=(1280,720) if d['board']=='tab5' else (1024,600)
 asset=bytearray(w*h*2)
 for y in range(h):
  for x in range(w):struct.pack_into('<H',asset,(y*w+x)*2,(x*31//w)<<11 | (y*63//h)<<5 | (((x//48)^(y//48))&31))
 at=buf.find(asset);assert at>=0,(p,'asset absent')
 records.append({'board':d['board'],'variant':v,'config_verified':True,'wallpaper_exact_bytes_offset':at,'firmware_sha256':d['firmware_sha256']})
(ROOT/'binary-verification.json').write_text(json.dumps(records,indent=2)+'\n')
print(json.dumps({'verified_binaries':len(records),'physical_results':'pending'}))
