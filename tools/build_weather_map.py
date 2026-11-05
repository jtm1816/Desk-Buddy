from pathlib import Path
import json, math, struct, gzip
from PIL import Image, ImageDraw, ImageFont

project_root=Path(__file__).resolve().parents[1]
root=project_root/'assets/map-source'; size=192
lat,lon=33.4706,-82.5009
span=30*1609.344/(40075016.686*math.cos(math.radians(lat))/16384)
def world(la,lo):
    r=math.radians(la)
    return ((lo+180)/360*16384,(1-math.asinh(math.tan(r))/math.pi)/2*16384)
cx,cy=world(lat,lon)
def project(la,lo):
    x,y=world(la,lo)
    return (95.5+(x-cx)*size/span,95.5+(y-cy)*size/span)
im=Image.new('RGB',(size,size)); d=ImageDraw.Draw(im)
features=json.loads(gzip.open(root/'thomson-osm.json.gz','rt',encoding='utf-8').read())['elements']
for feature in features:
    if feature['type']!='relation' or feature.get('tags',{}).get('natural')!='water':continue
    parts=[m['geometry'][:] for m in feature.get('members',[]) if m.get('role')=='outer' and len(m.get('geometry',[]))>=2]
    while parts:
        ring=parts.pop()
        while ring[0]!=ring[-1]:
            for i,part in enumerate(parts):
                if ring[-1]==part[0]:ring+=part[1:];parts.pop(i);break
                if ring[-1]==part[-1]:ring+=list(reversed(part))[1:];parts.pop(i);break
            else:break
        if ring[0]==ring[-1]:
            points=[project(p['lat'],p['lon']) for p in ring]
            area=abs(sum(a[0]*b[1]-b[0]*a[1] for a,b in zip(points,points[1:])))/2
            if area>=8:d.polygon(points,fill=(6,28,40))
for feature in features:
    if feature['type']!='way':continue
    points=[project(p['lat'],p['lon']) for p in feature.get('geometry',[])]
    if len(points)<2:continue
    tags=feature.get('tags',{})
    if tags.get('natural')=='water':
        area=abs(sum(a[0]*b[1]-b[0]*a[1] for a,b in zip(points,points[1:]+points[:1])))/2
        if area>=8:d.polygon(points,fill=(6,28,40));d.line(points,fill=(18,49,65))
for feature in features:
    if feature['type']!='way':continue
    points=[project(p['lat'],p['lon']) for p in feature.get('geometry',[])]
    if len(points)<2:continue
    tags=feature.get('tags',{})
    if tags.get('waterway')=='river':d.line(points,fill=(18,49,65))
    elif 'highway' in tags:
        level=tags['highway'];d.line(points,fill=(152,168,180) if level in ('motorway','trunk') else (96,112,124))
font=ImageFont.truetype('C:/Windows/Fonts/arial.ttf',9)
mask=Image.new('1',(size,size));md=ImageDraw.Draw(mask)
places={f.get('tags',{}).get('name'):f for f in features if f['type']=='node'}
labels=['Thomson','Warrenton','Dearing','Harlem','Appling','Grovetown','Augusta','Washington','Wrens','Waynesboro','Sandersville','Louisville','Lincolnton','Harlem']
occupied=[]
for name in labels:
    f=places.get(name)
    if not f:continue
    x,y=project(f['lat'],f['lon'])
    if math.hypot(x-95.5,y-95.5)>88:continue
    w=d.textlength(name,font=font);xx=max(4,min(size-w-4,x+4));yy=y-10
    if name=='Thomson':xx=x+6;yy=y+3
    edge=max(abs(yy-95.5),abs(yy+11-95.5))
    chord=math.sqrt(max(0,94**2-edge**2))
    if chord*2<w+4:continue
    xx=max(95.5-chord+2,min(95.5+chord-w-2,xx))
    box=(xx-1,yy-1,xx+w+1,yy+11)
    if any(not(box[2]<b[0] or box[0]>b[2] or box[3]<b[1] or box[1]>b[3]) for b in occupied):continue
    occupied.append(box)
    d.text((xx,yy),name,font=font,fill='white',stroke_width=1,stroke_fill='black')
    md.text((xx,yy),name,font=font,fill=1,stroke_width=1,stroke_fill=1)
    d.ellipse((x-1,y-1,x+1,y+1),fill=(180,190,195));md.ellipse((x-1,y-1,x+1,y+1),fill=1)
for y in range(size):
    for x in range(size):
        if (x-95.5)**2+(y-95.5)**2>96**2:
            im.putpixel((x,y),(8,20,24));mask.putpixel((x,y),0)
im.save(root/'thomson-basemap.png')
values=[((r>>3)<<11)|((g>>2)<<5)|(b>>3) for r,g,b in im.getdata()]
colors=sorted(set(values));assert len(colors)<=256
lookup={color:i for i,color in enumerate(colors)}
indices=[lookup[value] for value in values]
assert [colors[index] for index in indices]==values
maskbytes=mask.tobytes()
header='''#pragma once
#include <Arduino.h>
// Thomson, Georgia: 33.4706,-82.5009. Web Mercator, north up, 15-mile radius.
// Map data (c) OpenStreetMap contributors, ODbL. https://www.openstreetmap.org/copyright
// Downloaded November 5, 2026. Matches the centered zoom-6 RainViewer crop.
static constexpr float WR_MAP_LAT = 33.4706f, WR_MAP_LON = -82.5009f;
static const uint16_t WR_BASEMAP_PALETTE[] PROGMEM = {
'''
header+=','.join(hex(v) for v in colors)+'};\n'
header+='static const uint8_t WR_BASEMAP_INDEX[] PROGMEM = {\n'
header+=''.join(','.join(hex(v) for v in indices[i:i+32])+',\n' for i in range(0,len(indices),32))+'};\n'
header+='static const uint8_t WR_LABEL_MASK[] PROGMEM = {\n'
header+=''.join(','.join(hex(v) for v in maskbytes[i:i+32])+',\n' for i in range(0,len(maskbytes),32))+'};\n'
(project_root/'firmware/Deskbuddy_weather/WeatherRadarMap.h').write_text(header)
print('Thomson basemap generated:',len(indices)+len(colors)*2+len(maskbytes),'flash bytes; span:',round(span,3))
print('Indexed basemap verified lossless:',len(colors),'RGB565 colors')

# The aircraft view keeps its existing compass/status layout, using a nearest-
# neighbour copy of this exact same map inside its 220-pixel radar circle.
plane=im.resize((220,220),Image.Resampling.NEAREST)
plane_mask=mask.resize((220,220),Image.Resampling.NEAREST)
runs=[]
for y in range(220):
    x=0
    while x<220:
        r,g,b=plane.getpixel((x,y));value=((r>>3)<<11)|((g>>2)<<5)|(b>>3)
        inside=(x-109.5)**2+(y-109.5)**2<=110**2
        visible=inside and (value!=0 or plane_mask.getpixel((x,y)))
        if not visible:x+=1;continue
        start=x;index=lookup[value];x+=1
        while x<220:
            r,g,b=plane.getpixel((x,y));v=((r>>3)<<11)|((g>>2)<<5)|(b>>3)
            if v!=value or (x-109.5)**2+(y-109.5)**2>110**2:break
            x+=1
        runs.append((y,start,x-start,index))
text='#pragma once\n#include "WeatherRadarMap.h"\n// Same Thomson basemap, fixed 15-mile radius, scaled for the aircraft view.\n'
text+='static const uint8_t PLANE_MAP_RUNS[][4] PROGMEM = {\n'
text+=''.join('{%d,%d,%d,%d},\n'%run for run in runs)+'};\n'
(project_root/'firmware/Deskbuddy_weather/PlaneRadarMap.h').write_text(text)
plane.save(root/'plane-shared-basemap.png')
print('Shared aircraft map feature runs:',len(runs),'flash bytes:',len(runs)*4)

