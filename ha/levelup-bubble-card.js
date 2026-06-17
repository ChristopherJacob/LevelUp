/**
 * LevelUp Bubble Card — Clean spirit-level for Home Assistant
 * 
 * usage: type: custom:levelup-bubble-card
 *   roll_entity: sensor.levelup_roll
 *   pitch_entity: sensor.levelup_pitch
 */
class LevelUpBubbleCard extends HTMLElement {

  static getStubConfig() {
    return { roll_entity: 'sensor.levelup_roll', pitch_entity: 'sensor.levelup_pitch' };
  }

  setConfig(c) {
    if (!c.roll_entity || !c.pitch_entity) throw new Error('roll_entity/pitch_entity required');
    this._cfg = Object.assign({ tolerance: 1.5, title: '', multiplier: 4, size: 240 }, c);
  }

  set hass(h) {
    if (!this._built) { this._build(); this._built = true; }
    const r = parseFloat(h.states[this._cfg.roll_entity]?.state) || 0;
    const p = parseFloat(h.states[this._cfg.pitch_entity]?.state) || 0;
    this._upd(r, p);
  }

  _build() {
    const { title: t, size: s } = this._cfg;
    const h = Math.floor(s/2), r = Math.floor(s*0.44), ri = Math.floor(s*0.40);
    this.innerHTML = `<ha-card><div style="display:flex;flex-direction:column;align-items:center;padding:16px 8px;gap:8px;color:var(--primary-text-color,#ccc);background:var(--card-background-color,#1c1c1e);border-radius:12px">
      ${t?`<div style="font-size:13px;font-weight:600;color:var(--secondary-text-color,#888);text-transform:uppercase;letter-spacing:0.5px">${t}</div>`:''}
      <svg id="lbc-svg" width="${s}" height="${s}" viewBox="-${h} -${h} ${s} ${s}" style="display:block">
        <circle cx="0" cy="0" r="${r}" fill="none" stroke="#333" stroke-width="2"/>
        <circle cx="0" cy="0" r="${ri}" fill="none" stroke="#2a2a2a" stroke-width="1"/>
        <line x1="0" y1="-${ri+3}" x2="0" y2="${ri+3}" stroke="#2a2a2a" stroke-width="0.5"/>
        <line x1="-${ri+3}" y1="0" x2="${ri+3}" y2="0" stroke="#2a2a2a" stroke-width="0.5"/>
        <line x1="0" y1="-${r}" x2="0" y2="-${r-7}" stroke="#333" stroke-width="1"/>
        <line x1="0" y1="${r}" x2="0" y2="${r-7}" stroke="#333" stroke-width="1"/>
        <line x1="-${r}" y1="0" x2="-${r-7}" y2="0" stroke="#333" stroke-width="1"/>
        <line x1="${r}" y1="0" x2="${r-7}" y2="0" stroke="#333" stroke-width="1"/>
        <circle cx="0" cy="0" r="2.5" fill="#555"/>
        <circle id="lbc-dot" cx="0" cy="0" r="16" fill="#1f6feb" style="filter:drop-shadow(0 0 8px rgba(31,111,235,0.5))"/>
      </svg>
      <div id="lbc-status" style="font-size:12px;font-weight:600;color:#888;letter-spacing:0.5px">--</div>
      <div style="display:flex;gap:20px">
        <div style="display:flex;flex-direction:column;align-items:center">
          <span style="font-size:9px;color:#666;text-transform:uppercase">Roll</span>
          <span id="lbc-roll" style="font-family:monospace;font-size:16px;font-weight:500">--°</span>
        </div>
        <div style="display:flex;flex-direction:column;align-items:center">
          <span style="font-size:9px;color:#666;text-transform:uppercase">Pitch</span>
          <span id="lbc-pitch" style="font-family:monospace;font-size:16px;font-weight:500">--°</span>
        </div>
      </div>
    </div></ha-card>`;
  }

  _upd(roll, pitch) {
    const dot = this.querySelector('#lbc-dot');
    const st = this.querySelector('#lbc-status');
    const rv = this.querySelector('#lbc-roll');
    const pv = this.querySelector('#lbc-pitch');
    if (!dot) return;

    const { tolerance: tol, multiplier: mul } = this._cfg;
    const sc = 2.5 * mul, max = Math.floor(this._cfg.size * 0.44) - 16;
    const bx = Math.max(-max, Math.min(max, roll * sc));
    const by = Math.max(-max, Math.min(max, -pitch * sc));
    const lvl = Math.abs(roll) <= tol && Math.abs(pitch) <= tol;

    dot.setAttribute('cx', bx);
    dot.setAttribute('cy', by);
    dot.setAttribute('fill', lvl ? '#34c759' : '#1f6feb');
    dot.setAttribute('style', lvl ? 'filter:drop-shadow(0 0 12px rgba(52,199,89,0.6))' : 'filter:drop-shadow(0 0 8px rgba(31,111,235,0.5))');
    dot.setAttribute('r', lvl ? '18' : '16');

    st.textContent = lvl ? '● LEVEL' : `○ ${Math.max(Math.abs(roll), Math.abs(pitch)).toFixed(1)}° off`;
    st.style.color = lvl ? '#34c759' : '#888';

    rv.textContent = roll.toFixed(1) + '°';
    pv.textContent = pitch.toFixed(1) + '°';
  }

  getCardSize() { return 3; }
}
customElements.define('levelup-bubble-card', LevelUpBubbleCard);
