<!doctype html>
<!-- File: index.html
    Simple in-browser 4-stem splitter (approximate stems via filter bands) with 4 volume sliders.
    Usage: open this file in a modern browser, load an audio file, Play, adjust sliders,
    or export each stem as a WAV file.
-->
<html>
<head>
  <meta charset="utf-8" />
  <title>4-Stem Splitter</title>
  <style>
    body { font-family: Arial, sans-serif; padding: 20px; max-width: 800px; }
    .controls { margin-top: 12px; display: grid; gap: 10px; }
    .stem { display:flex; align-items:center; gap:12px; }
    .stem label { min-width:90px; }
    input[type="range"] { width: 320px; }
    button { padding: 8px 12px; }
    .row { display:flex; gap:8px; align-items:center; margin-top:10px; }
  </style>
</head>
<body>
  <h2>4-Stem Splitter (Band-based)</h2>
  <p>Loads an audio file and splits it into 4 frequency-band stems (low / low-mid / high-mid / high).
    Use sliders to adjust volume per stem in real-time. Export stems to WAV if desired.</p>

  <input id="fileInput" type="file" accept="audio/*">
  <div class="row">
    <button id="playBtn" disabled>Play</button>
    <button id="stopBtn" disabled>Stop</button>
    <button id="exportAllBtn" disabled>Export All Stems (WAV)</button>
  </div>

  <div class="controls" id="stems">
    <div class="stem">
     <label>Low (bass)</label>
     <input id="gain0" type="range" min="0" max="2" step="0.01" value="1">
     <span id="val0">1.00</span>
     <button id="download0" disabled>Download</button>
    </div>
    <div class="stem">
     <label>Low-Mid</label>
     <input id="gain1" type="range" min="0" max="2" step="0.01" value="1">
     <span id="val1">1.00</span>
     <button id="download1" disabled>Download</button>
    </div>
    <div class="stem">
     <label>High-Mid</label>
     <input id="gain2" type="range" min="0" max="2" step="0.01" value="1">
     <span id="val2">1.00</span>
     <button id="download2" disabled>Download</button>
    </div>
    <div class="stem">
     <label>High (treble)</label>
     <input id="gain3" type="range" min="0" max="2" step="0.01" value="1">
     <span id="val3">1.00</span>
     <button id="download3" disabled>Download</button>
    </div>
  </div>

  <script>
    // GitHub Copilot
    let audioCtx = null;
    let buffer = null;
    let src = null;
    const stemGains = [];
    const filters = [];
    const filterParams = [
     { type: 'lowpass', freq: 250, q: 0.8 },      // Low / bass
     { type: 'bandpass', freq: 800, q: 1.0 },     // Low-mid
     { type: 'bandpass', freq: 3000, q: 1.0 },    // High-mid
     { type: 'highpass', freq: 6000, q: 0.8 }     // High / treble
    ];

    const fileInput = document.getElementById('fileInput');
    const playBtn = document.getElementById('playBtn');
    const stopBtn = document.getElementById('stopBtn');
    const exportAllBtn = document.getElementById('exportAllBtn');

    fileInput.addEventListener('change', async (e) => {
     const f = e.target.files && e.target.files[0];
     if (!f) return;
     if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
     const ab = await f.arrayBuffer();
     buffer = await audioCtx.decodeAudioData(ab);
     enableControls(true);
    });

    function enableControls(enabled) {
     playBtn.disabled = !enabled;
     stopBtn.disabled = !enabled;
     exportAllBtn.disabled = !enabled;
     for (let i = 0; i < 4; ++i) {
       document.getElementById('download' + i).disabled = !enabled;
     }
    }

    playBtn.addEventListener('click', () => {
     if (!buffer) return;
     startPlayback();
    });
    stopBtn.addEventListener('click', stopPlayback);

    // Create persistent filter + gain nodes so sliders control live gains.
    function createStemNodes() {
     // cleanup old
     for (let g of stemGains) if (g) try { g.disconnect(); } catch(e){}
     for (let f of filters) if (f) try { f.disconnect(); } catch(e){}
     stemGains.length = 0; filters.length = 0;
     for (let i = 0; i < 4; ++i) {
       const gainNode = audioCtx.createGain();
       gainNode.gain.value = parseFloat(document.getElementById('gain' + i).value);
       const fil = audioCtx.createBiquadFilter();
       fil.type = filterParams[i].type;
       fil.frequency.value = filterParams[i].freq;
       fil.Q.value = filterParams[i].q;
       filters.push(fil);
       stemGains.push(gainNode);
       // chain: source -> filter -> gain -> destination (mix)
       gainNode.connect(audioCtx.destination);
     }
    }

    function startPlayback() {
     if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
     if (audioCtx.state === 'suspended') audioCtx.resume();
     createStemNodes();
     // create fresh source
     src = audioCtx.createBufferSource();
     src.buffer = buffer;
     // Connect source to each filter -> gain
     for (let i = 0; i < 4; ++i) {
       const f = filters[i];
       const g = stemGains[i];
       src.connect(f);
       f.connect(g);
     }
     src.start();
     playBtn.disabled = true;
     stopBtn.disabled = false;
     src.onended = () => {
       playBtn.disabled = false;
       stopBtn.disabled = true;
     };
    }

    function stopPlayback() {
     if (src) {
       try { src.stop(); } catch(e){}
       src = null;
     }
     playBtn.disabled = false;
     stopBtn.disabled = true;
    }

    // Wire sliders to gain nodes (real-time)
    for (let i = 0; i < 4; ++i) {
     const slider = document.getElementById('gain' + i);
     const label = document.getElementById('val' + i);
     slider.addEventListener('input', (e) => {
       const v = parseFloat(e.target.value);
       label.textContent = v.toFixed(2);
       if (stemGains[i]) stemGains[i].gain.setTargetAtTime(v, (audioCtx && audioCtx.currentTime) || 0, 0.01);
     });
    }

    // Export stems by rendering each filter chain offline and creating WAV blob.
    exportAllBtn.addEventListener('click', async () => {
     if (!buffer) return;
     exportAllBtn.disabled = true;
     for (let i = 0; i < 4; ++i) {
       const blob = await renderStemOffline(i);
       triggerDownload(blob, `stem-${i}-${filterParams[i].type}.wav`);
     }
     exportAllBtn.disabled = false;
    });

    for (let i = 0; i < 4; ++i) {
     document.getElementById('download' + i).addEventListener('click', async () => {
       if (!buffer) return;
       const blob = await renderStemOffline(i);
       triggerDownload(blob, `stem-${i}-${filterParams[i].type}.wav`);
     });
    }

    async function renderStemOffline(index) {
     const sampleRate = audioCtx ? audioCtx.sampleRate : 44100;
     const length = buffer.length;
     const offline = new OfflineAudioContext(buffer.numberOfChannels, length, sampleRate);
     const srcNode = offline.createBufferSource();
     srcNode.buffer = buffer;
     const fil = offline.createBiquadFilter();
     fil.type = filterParams[index].type;
     fil.frequency.value = filterParams[index].freq;
     fil.Q.value = filterParams[index].q;
     const gainNode = offline.createGain();
     // use current slider value to bake volume into export
     const sliderVal = parseFloat(document.getElementById('gain' + index).value);
     gainNode.gain.value = sliderVal;
     srcNode.connect(fil);
     fil.connect(gainNode);
     gainNode.connect(offline.destination);
     srcNode.start(0);
     const rendered = await offline.startRendering();
     return bufferToWavBlob(rendered);
    }

    // WAV encoding (PCM16)
    function bufferToWavBlob(buffer) {
     const numOfChan = buffer.numberOfChannels;
     const length = buffer.length * numOfChan * 2 + 44;
     const bufferArray = new ArrayBuffer(length);
     const view = new DataView(bufferArray);

     /* RIFF identifier */
     writeString(view, 0, 'RIFF');
     /* file length */
     view.setUint32(4, 36 + buffer.length * numOfChan * 2, true);
     /* RIFF type */
     writeString(view, 8, 'WAVE');
     /* format chunk identifier */
     writeString(view, 12, 'fmt ');
     /* format chunk length */
     view.setUint32(16, 16, true);
     /* sample format (raw) */
     view.setUint16(20, 1, true);
     /* channel count */
     view.setUint16(22, numOfChan, true);
     /* sample rate */
     view.setUint32(24, buffer.sampleRate, true);
     /* byte rate (sampleRate * blockAlign) */
     view.setUint32(28, buffer.sampleRate * numOfChan * 2, true);
     /* block align (channel count * bytes per sample) */
     view.setUint16(32, numOfChan * 2, true);
     /* bits per sample */
     view.setUint16(34, 16, true);
     /* data chunk identifier */
     writeString(view, 36, 'data');
     /* data chunk length */
     view.setUint32(40, buffer.length * numOfChan * 2, true);

     // write interleaved PCM16
     let offset = 44;
     const channels = [];
     for (let i = 0; i < numOfChan; i++) channels.push(buffer.getChannelData(i));
     for (let i = 0; i < buffer.length; i++) {
       for (let ch = 0; ch < numOfChan; ch++) {
        let sample = Math.max(-1, Math.min(1, channels[ch][i]));
        sample = sample < 0 ? sample * 0x8000 : sample * 0x7FFF;
        view.setInt16(offset, sample, true);
        offset += 2;
       }
     }

     return new Blob([view], { type: 'audio/wav' });
    }

    function writeString(view, offset, string) {
     for (let i = 0; i < string.length; i++) {
       view.setUint8(offset + i, string.charCodeAt(i));
     }
    }

    function triggerDownload(blob, filename) {
     const url = URL.createObjectURL(blob);
     const a = document.createElement('a');
     a.style = 'display:none';
     a.href = url;
     a.download = filename;
     document.body.appendChild(a);
     a.click();
     setTimeout(() => {
       document.body.removeChild(a);
       URL.revokeObjectURL(url);
     }, 100);
    }
  </script>
</body>
</html>