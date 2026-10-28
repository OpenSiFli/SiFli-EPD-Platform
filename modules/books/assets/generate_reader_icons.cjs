const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');
const shapes = [
 '<path d="m15 4-8 8 8 8"/>',
 '<path d="m4 8 8 8 8-8"/>',
 '<path d="m5 5 14 14M19 5 5 19"/>',
 '<path d="M8 5h13M8 12h13M8 19h13M3 5h.1M3 12h.1M3 19h.1"/>',
 '<path d="M4 6h16M4 18h16M9 3v6M16 15v6"/><circle cx="16" cy="18" r="3"/><circle cx="9" cy="6" r="3"/>',
 '<path d="m12 3 2.5 6.5L21 12l-6.5 2.5L12 21l-2.5-6.5L3 12l6.5-2.5ZM20 2v4M18 4h4"/>',
 '<circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M2 12h2M20 12h2M5 5l1.5 1.5M17.5 17.5 19 19M5 19l1.5-1.5M17.5 6.5 19 5"/>',
 '<path d="m4 20 8-16 8 16M7 14h10"/>',
 '<path d="M4 12h16"/>',
 '<path d="M4 12h16M12 4v16"/>',
 '<rect x="9" y="2" width="6" height="13" rx="3"/><path d="M5 10v2a7 7 0 0 0 14 0v-2M12 19v3M8 22h8"/>',
 '<path d="M12 21V3M5 10l7-7 7 7"/>',
 '<path d="M10 5 5 9H2v6h3l5 4ZM15 8a6 6 0 0 1 0 8M18 4a11 11 0 0 1 0 16"/>',
 '<rect x="5" y="5" width="14" height="14" rx="1"/>',
 '<path d="m9 4 8 8-8 8"/>',
 '<path d="M21 4v6h-6M3 20v-6h6M3.5 9a9 9 0 0 1 15-5L21 7M3 17l2.5 3a9 9 0 0 0 15-5"/>'
];
(async()=>{
 let c='#include "reader_icons.h"\n';
 for(let n=0;n<shapes.length;n++){
  const svg=`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" width="48" height="48" fill="none" stroke="black" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">${shapes[n]}</svg>`;
  const raw=await sharp(Buffer.from(svg)).ensureAlpha().raw().toBuffer();
  const data=Buffer.alloc(64+48*24);
  for(let i=0;i<16;i++)data[i*4+3]=i*17;
  for(let i=0;i<48*48;i++)data[64+(i>>1)]|=Math.round(raw[i*4+3]/17)<<(i%2?0:4);
  c+=`static const uint8_t pixels_${n}[]={\n`;
  for(let i=0;i<data.length;i+=24)c+=' '+[...data.subarray(i,i+24)].join(',')+',\n';
  c+=`};\nstatic const lv_image_dsc_t icon_${n}={.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_I4,.w=48,.h=48,.stride=24},.data_size=sizeof(pixels_${n}),.data=pixels_${n}};\n`;
 }
 c+='const lv_image_dsc_t *book_icon_source(book_icon_t icon){\n static const lv_image_dsc_t *const icons[]={'+shapes.map((_,i)=>'&icon_'+i).join(',')+'};\n return (unsigned)icon<BOOK_ICON_COUNT?icons[icon]:NULL;\n}\n';
 fs.writeFileSync(path.join(__dirname,'../reader_icons.c'),c);
})();
