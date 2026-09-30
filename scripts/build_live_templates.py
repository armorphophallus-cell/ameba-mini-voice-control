"""Replace spectral templates with features captured from the AMB82 microphone."""
from pathlib import Path
import json
import numpy as np

ROOT=Path(__file__).resolve().parents[1]
rows=[]
for line in (ROOT/'model/artifacts/live_calibration.csv').read_text(encoding='utf-8-sig').splitlines():
    parts=line.strip().split(',')
    if len(parts)==387 and parts[0]=='CAL': rows.append((parts[1],int(parts[2]),np.array(parts[3:],dtype=np.float32)))
groups={label:np.stack([v for lab,_,v in rows if lab==label]) for label in ('L','R')}
templates={label:values.mean(axis=0) for label,values in groups.items()}
checks=[]
for label in ('L','R'):
    for i,v in enumerate(groups[label]):
        own=float(np.mean((v-templates[label])**2)); other=float(np.mean((v-templates['R' if label=='L' else 'L'])**2))
        checks.append({'label':label,'index':i+1,'own':own,'other':other,'margin':other-own,'correct':own<other})
own=[x['own'] for x in checks]; margins=[x['margin'] for x in checks]
max_distance=max(own)*1.6
positive=[m for m in margins if m>0]
min_margin=max(0.001,min(positive)*0.35) if positive else 1.0
def array(name,values):
    out=[]
    for i in range(0,len(values),8): out.append('    '+', '.join((f'{x:.8g}' if '.' in f'{x:.8g}' or 'e' in f'{x:.8g}' else f'{x:.8g}.0')+'f' for x in values[i:i+8]))
    return f'static const float {name}[SPECTRAL_FEATURES] = {{\n'+',\n'.join(out)+'\n};\n'
header=f'''#pragma once
constexpr int SPECTRAL_FRAMES=32;
constexpr int SPECTRAL_BANDS=12;
constexpr int SPECTRAL_FEATURES=SPECTRAL_FRAMES*SPECTRAL_BANDS;
constexpr float SPECTRAL_MAX_DISTANCE={max_distance:.8g}f;
constexpr float SPECTRAL_MIN_MARGIN={min_margin:.8g}f;
static const int SPECTRAL_FREQUENCIES[SPECTRAL_BANDS]={{250,400,550,700,900,1100,1350,1650,2000,2400,2900,3500}};
'''+array('LEFT_TEMPLATE',templates['L'])+array('RIGHT_TEMPLATE',templates['R'])
(ROOT/'spectral_templates.h').write_text(header,encoding='ascii')
report={'samples':{k:len(v) for k,v in groups.items()},'checks':checks,'max_distance':max_distance,'min_margin':min_margin,'all_closer_to_own':all(x['correct'] for x in checks)}
(ROOT/'model/artifacts/live_template_report.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print(json.dumps(report,indent=2))
