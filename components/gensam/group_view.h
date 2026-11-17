#pragma once

/// @file group_view.h
/// @brief Shared browser renderer for uploaded and active group preset details.
///
/// The renderer is served as same-origin JavaScript so both the upload preview and main page
/// use identical PEQ tables. Values describe presets, not readback.

namespace esphome {
namespace gensam {

/// Shared JavaScript for rendering a group's expandable device settings.
static const char GROUP_VIEW_JS[] = R"JS(
window.gensamRenderGroup = function(g) {
const node=(tag,text)=>{const e=document.createElement(tag);if(text!==undefined)e.textContent=text;return e};
const group=node('div');
for(const d of g.devices){
const speaker=node('details');speaker.append(node('summary',(d.name||'Monitor '+d.id)+' — '+(d.enabled?'enabled':'disabled')));
const settings=node('dl');
for(const [label,value] of [['Input',d.input],['Crossover',d.crossover],['Level',Number(d.level_db)+' dB'],['Delay',d.delay_samples+' samples ('+Number(d.delay_ms).toFixed(2)+' ms)'],['LFE',d.lfe+(d.lfe==='none'?'':' ('+Number(d.lfe_level_db)+' dB)')]])settings.append(node('dt',label),node('dd',value));
speaker.append(settings);
const bands=d.peq_bands||[],eq=node('details');eq.append(node('summary','PEQ '+d.peq_active+'/'+bands.length+' active'));
eq.append(node('p',d.peq_design_rate_hz?'Filter design rate: '+d.peq_design_rate_hz+' Hz':'Filter design rate: monitor default'));
eq.append(node('p','Shelf Q values are fixed by the filter design.'));
const wrap=node('div');wrap.className='table-wrap';const table=node('table'),head=node('thead'),header=node('tr');
for(const label of ['Slot (1-based)','Type','Status','Frequency (Hz)','Gain (dB)','Q'])header.append(node('th',label));
head.append(header);table.append(head);const body=node('tbody');
bands.forEach((b,i)=>{const row=node('tr'),bypass=b===null;
for(const value of [String(i+1),bypass?'Bypass':b.type,bypass||Number(b.gain_db)===0?'Bypassed':'Active',bypass?'—':String(b.frequency_hz),bypass?'—':String(b.gain_db),bypass?'—':b.type==='Peaking'?String(b.q):b.effective_q!=null?String(Number(Number(b.effective_q).toPrecision(6)))+' (fixed)':'—'])row.append(node('td',value));
body.append(row)});
table.append(body);wrap.append(table);eq.append(wrap);speaker.append(eq);group.append(speaker)
}
return group;
};
)JS";

}  // namespace gensam
}  // namespace esphome
