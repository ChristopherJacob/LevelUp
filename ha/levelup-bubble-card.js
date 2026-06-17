/**
 * LevelUp Bubble Card — Clean spirit-level display for Home Assistant
 * 
 * Usage:
 *   type: custom:levelup-bubble-card
 *   roll_entity: sensor.levelup_roll
 *   pitch_entity: sensor.levelup_pitch
 *   tolerance: 1.5
 */
class LevelUpBubbleCard extends HTMLElement {

  static getStubConfig() {
    return { roll_entity: 'sensor.levelup_roll', pitch_entity: 'sensor.levelup_pitch' };
  }

  setConfig(config) {
    if (!config.roll_entity || !config.pitch_entity) {
      throw new Error('roll_entity and pitch_entity are required');
    }
    this._config = Object.assign({
      tolerance: 1.5,
      title: '',
      multiplier: 4,
      size: 240
    }, config);
  }

  set hass(hass) {
    this._hass = hass;
    if (!this._built) {
      this._build();
      this._built = true;
    }
    const roll = parseFloat(hass.states[this._config.roll_entity]?.state) || 0;
    const pitch = parseFloat(hass.states[this._config.pitch_entity]?.state) || 0;
    this._update(roll, pitch);
  }

  _build() {
    const { title, size } = this._config;
    this.innerHTML = `
      <ha-card>
        <div style="display:flex;flex-direction:column;align-items:center;padding:16px;gap:8px">
          ${title ? `<div class="lbc-title">${title}</div>` : ''}
          <div class="lbc-ring-wrap" style="width:${size}px;height:${size}px">
            <svg class="lbc-svg" viewBox="-120 -120 240 240">
              <circle cx="0" cy="0" r="105" class="lbc-ring-outer"/>
              <circle cx="0" cy="0" r="95" class="lbc-ring-inner"/>
              <line x1="0" y1="-98" x2="0" y2="98" class="lbc-crosshair"/>
              <line x1="-98" y1="0" x2="98" y2="0" class="lbc-crosshair"/>
              <g class="lbc-tick">
                <line x1="0" y1="-95" x2="0" y2="-88" /><line x1="0" y1="95" x2="0" y2="88"/>
                <line x1="-95" y1="0" x2="-88" y2="0" /><line x1="95" y1="0" x2="88" y2="0"/>
              </g>
              <circle cx="0" cy="0" r="3" class="lbc-center"/>
              <circle cx="0" cy="0" r="16" class="lbc-bubble" id="bubble"/>
            </svg>
          </div>
          <div class="lbc-status" id="status">--</div>
          <div class="lbc-values">
            <div class="lbc-val"><span class="lbc-vlabel">Roll</span><span class="lbc-vnum" id="rval">--°</span></div>
            <div class="lbc-val"><span class="lbc-vlabel">Pitch</span><span class="lbc-vnum" id="pval">--°</span></div>
          </div>
        </div>
      </ha-card>
    `;
  }

  _update(roll, pitch) {
    const bubble = this.querySelector('#bubble');
    const status = this.querySelector('#status');
    const rval = this.querySelector('#rval');
    const pval = this.querySelector('#pval');
    if (!bubble) return;

    const { tolerance, multiplier } = this._config;
    const scale = 2.5 * multiplier;
    const bx = Math.max(-95, Math.min(95, roll * scale));
    const by = Math.max(-95, Math.min(95, -pitch * scale));
    const isLevel = Math.abs(roll) <= tolerance && Math.abs(pitch) <= tolerance;

    bubble.setAttribute('cx', bx);
    bubble.setAttribute('cy', by);
    bubble.classList.toggle('lbc-level', isLevel);

    status.textContent = isLevel ? '● LEVEL' : `○ ${Math.max(Math.abs(roll), Math.abs(pitch)).toFixed(1)}°`;
    status.className = 'lbc-status' + (isLevel ? ' lbc-level' : '');

    rval.textContent = roll.toFixed(1) + '°';
    pval.textContent = pitch.toFixed(1) + '°';
  }

  getCardSize() { return 3; }
}

customElements.define('levelup-bubble-card', LevelUpBubbleCard);

// Styles injected once
if (!document.getElementById('lbc-styles')) {
  const s = document.createElement('style');
  s.id = 'lbc-styles';
  s.textContent = `
    .lbc-title { font-size:13px;font-weight:600;color:var(--secondary-text-color);text-transform:uppercase;letter-spacing:0.5px }
    .lbc-ring-wrap { position:relative }
    .lbc-svg { width:100%;height:100% }
    .lbc-ring-outer { fill:none;stroke:var(--divider-color, rgba(128,128,128,0.15));stroke-width:2 }
    .lbc-ring-inner { fill:none;stroke:var(--divider-color, rgba(128,128,128,0.08));stroke-width:1 }
    .lbc-crosshair { stroke:var(--divider-color, rgba(128,128,128,0.06));stroke-width:0.5 }
    .lbc-tick line { stroke:var(--divider-color, rgba(128,128,128,0.1));stroke-width:1 }
    .lbc-center { fill:var(--secondary-text-color);opacity:0.2 }
    .lbc-bubble { fill:var(--primary-color, #1f6feb);filter:drop-shadow(0 0 6px rgba(31,111,235,0.5));transition:cx 0.12s linear,cy 0.12s linear,fill 0.5s ease,filter 0.5s ease }
    .lbc-bubble.lbc-level { fill:#34c759;filter:drop-shadow(0 0 10px rgba(52,199,89,0.6)) }
    .lbc-status { font-size:13px;font-weight:600;color:var(--secondary-text-color);letter-spacing:0.5px;transition:color 0.4s }
    .lbc-status.lbc-level { color:#34c759 }
    .lbc-values { display:flex;gap:20px }
    .lbc-val { display:flex;flex-direction:column;align-items:center }
    .lbc-vlabel { font-size:9px;color:var(--secondary-text-color);opacity:0.5;text-transform:uppercase }
    .lbc-vnum { font-family:monospace;font-size:16px;font-weight:500;color:var(--primary-text-color) }
  `;
  document.head.appendChild(s);
}
