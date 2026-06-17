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
    const s = size;
    
    this.innerHTML = `
      <style>
        :host { display:block }
        .lbc-wrap { display:flex;flex-direction:column;align-items:center;padding:16px 8px;gap:8px;box-sizing:border-box }
        .lbc-t { font-size:13px;font-weight:600;color:var(--secondary-text-color);text-transform:uppercase;letter-spacing:0.5px }
        .lbc-ring { position:relative;width:${s}px;height:${s}px }
        .lbc-svg { width:100%;height:100%;display:block }
        .lbc-ring-o { fill:none;stroke:var(--divider-color,#333);stroke-width:2 }
        .lbc-ring-i { fill:none;stroke:var(--divider-color,#333);stroke-width:1;opacity:0.5 }
        .lbc-x { stroke:var(--divider-color,#333);stroke-width:0.5;opacity:0.4 }
        .lbc-tick { stroke:var(--divider-color,#333);stroke-width:1;opacity:0.3 }
        .lbc-c { fill:var(--secondary-text-color,#666);opacity:0.25 }
        .lbc-b { fill:var(--primary-color,#1f6feb);filter:drop-shadow(0 0 6px rgba(31,111,235,0.5));transition:cx 0.12s linear,cy 0.12s linear,fill 0.5s ease,filter 0.5s ease }
        .lbc-b.lvl { fill:#34c759;filter:drop-shadow(0 0 10px rgba(52,199,89,0.6)) }
        .lbc-s { font-size:13px;font-weight:600;color:var(--secondary-text-color);letter-spacing:0.5px;transition:color 0.4s }
        .lbc-s.lvl { color:#34c759 }
        .lbc-v { display:flex;gap:20px }
        .lbc-vi { display:flex;flex-direction:column;align-items:center }
        .lbc-vl { font-size:9px;color:var(--secondary-text-color);opacity:0.5;text-transform:uppercase }
        .lbc-vn { font-family:monospace;font-size:16px;font-weight:500;color:var(--primary-text-color) }
      </style>
      <ha-card>
        <div class="lbc-wrap">
          ${title ? `<div class="lbc-t">${title}</div>` : ''}
          <div class="lbc-ring">
            <svg class="lbc-svg" viewBox="-120 -120 240 240">
              <circle cx="0" cy="0" r="105" class="lbc-ring-o"/>
              <circle cx="0" cy="0" r="95" class="lbc-ring-i"/>
              <line x1="0" y1="-98" x2="0" y2="98" class="lbc-x"/>
              <line x1="-98" y1="0" x2="98" y2="0" class="lbc-x"/>
              <line x1="0" y1="-95" x2="0" y2="-88" class="lbc-tick"/>
              <line x1="0" y1="95" x2="0" y2="88" class="lbc-tick"/>
              <line x1="-95" y1="0" x2="-88" y2="0" class="lbc-tick"/>
              <line x1="95" y1="0" x2="88" y2="0" class="lbc-tick"/>
              <circle cx="0" cy="0" r="3" class="lbc-c"/>
              <circle cx="0" cy="0" r="16" class="lbc-b"/>
            </svg>
          </div>
          <div class="lbc-s">--</div>
          <div class="lbc-v">
            <div class="lbc-vi"><span class="lbc-vl">Roll</span><span class="lbc-vn">--°</span></div>
            <div class="lbc-vi"><span class="lbc-vl">Pitch</span><span class="lbc-vn">--°</span></div>
          </div>
        </div>
      </ha-card>
    `;
  }

  _update(roll, pitch) {
    const bubble = this.querySelector('.lbc-b');
    const status = this.querySelector('.lbc-s');
    const vals = this.querySelectorAll('.lbc-vn');
    if (!bubble) return;

    const { tolerance, multiplier } = this._config;
    const scale = 2.5 * multiplier;
    const bx = Math.max(-95, Math.min(95, roll * scale));
    const by = Math.max(-95, Math.min(95, -pitch * scale));
    const isLevel = Math.abs(roll) <= tolerance && Math.abs(pitch) <= tolerance;

    bubble.setAttribute('cx', bx);
    bubble.setAttribute('cy', by);
    bubble.classList.toggle('lvl', isLevel);

    status.textContent = isLevel ? '● LEVEL' : `○ ${Math.max(Math.abs(roll), Math.abs(pitch)).toFixed(1)}° off`;
    status.classList.toggle('lvl', isLevel);

    if (vals.length >= 2) {
      vals[0].textContent = roll.toFixed(1) + '°';
      vals[1].textContent = pitch.toFixed(1) + '°';
    }
  }

  getCardSize() { return 3; }
}

customElements.define('levelup-bubble-card', LevelUpBubbleCard);
