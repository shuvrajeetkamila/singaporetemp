/* Singapore 1:1 (prototype) island viewer.
 * Terrain: Copernicus GLO-30 surface model, 60 m grid, SVY21 metres.
 * Buildings: OSM footprints extruded to estimated heights.
 * Coordinates: three.js x = east (m), y = height (m), z = -north (m), relative to the island origin.
 */
/* global THREE */
(function () {
  'use strict';

  var DATA = '../island/';
  var statusEl = document.getElementById('status');

  function setStatus(msg) { statusEl.textContent = msg; }
  function showError(e) { setStatus('Error: ' + (e && e.message ? e.message : e)); }
  function clamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }

  function fetchJson(url) {
    return fetch(url).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status + ' for ' + url);
      return r.json();
    });
  }
  function fetchBuf(url) {
    return fetch(url).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status + ' for ' + url);
      return r.arrayBuffer();
    });
  }

  // ---- renderer, scene, camera --------------------------------------------------
  var renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
  renderer.setSize(window.innerWidth, window.innerHeight);
  document.body.appendChild(renderer.domElement);

  var scene = new THREE.Scene();
  scene.background = new THREE.Color(0xa9d0f5);
  var camera = new THREE.PerspectiveCamera(50, window.innerWidth / window.innerHeight, 5, 300000);

  scene.add(new THREE.HemisphereLight(0xffffff, 0x556677, 0.9));
  var sun = new THREE.DirectionalLight(0xffffff, 0.7);
  sun.position.set(-0.5, 1.0, 0.3);
  scene.add(sun);

  var buildingGroup = new THREE.Group();
  scene.add(buildingGroup);
  var buildingMat = new THREE.MeshLambertMaterial({ color: 0xd9d4c7, side: THREE.DoubleSide, flatShading: true });

  // Orbit-style view state: target on the ground (cx, cy in metres from the origin).
  var view = { cx: 0, cy: 0, dist: 60000, phi: 1.0, theta: 0.0 };
  var MIN_DIST = 150;
  var MAX_DIST = 150000;

  function updateCamera() {
    var s = Math.sin(view.phi), c = Math.cos(view.phi);
    camera.position.set(
      view.cx + view.dist * s * Math.sin(view.theta),
      view.dist * c,
      -view.cy + view.dist * s * Math.cos(view.theta)
    );
    camera.lookAt(view.cx, 0, -view.cy);
  }

  // ---- terrain ------------------------------------------------------------------
  function heightColor(z) {
    if (z < 0.5) return [0.16, 0.42, 0.66];   // sea / water (DSM ~0 m)
    if (z < 5) return [0.62, 0.74, 0.46];     // coastal lowland
    if (z < 30) return [0.45, 0.66, 0.36];    // lowland
    if (z < 80) return [0.56, 0.52, 0.36];    // hills
    return [0.60, 0.52, 0.46];                // higher ground
  }

  function buildTerrain(buf, meta) {
    var W = meta.terrain.width, H = meta.terrain.height, cell = meta.terrain.cell_m;
    var h = new Float32Array(buf);
    if (h.length !== W * H) throw new Error('terrain size mismatch');
    var pos = new Float32Array(W * H * 3);
    var col = new Float32Array(W * H * 3);
    var j, i, k;
    for (j = 0; j < H; j++) {
      for (i = 0; i < W; i++) {
        k = j * W + i;
        var z = h[k];
        pos[3 * k] = i * cell;
        pos[3 * k + 1] = z;
        pos[3 * k + 2] = -j * cell;
        var c = heightColor(z);
        col[3 * k] = c[0]; col[3 * k + 1] = c[1]; col[3 * k + 2] = c[2];
      }
    }
    var idx = new Uint32Array((W - 1) * (H - 1) * 6);
    var p = 0;
    for (j = 0; j < H - 1; j++) {
      for (i = 0; i < W - 1; i++) {
        var a = j * W + i, b = a + 1, cc = a + W, d = cc + 1;
        idx[p++] = a; idx[p++] = b; idx[p++] = cc;
        idx[p++] = b; idx[p++] = d; idx[p++] = cc;
      }
    }
    var g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    g.setAttribute('color', new THREE.BufferAttribute(col, 3));
    g.setIndex(new THREE.BufferAttribute(idx, 1));
    g.computeVertexNormals();
    var mesh = new THREE.Mesh(g, new THREE.MeshLambertMaterial({ vertexColors: true }));
    scene.add(mesh);
    return mesh;
  }

  // ---- buildings ----------------------------------------------------------------
  // Binary format (see sg_pipeline/export_app_assets.py): "SGB1", uint32 count,
  // then per record: float32 base, float32 height, uint16 n, n * (float32 x, float32 y).
  // Each footprint becomes an indexed mesh: n roof vertices (shared by the roof triangles),
  // and 2n wall vertices (bottom and top of each outline vertex, shared by the wall quads).
  // Flat shading is done on the GPU, so no normal arrays are stored.
  function buildBuildings(buf, onProgress, onDone) {
    var dv = new DataView(buf);
    var magic = String.fromCharCode(dv.getUint8(0), dv.getUint8(1), dv.getUint8(2), dv.getUint8(3));
    if (magic !== 'SGB1') throw new Error('bad buildings.bin header');
    var count = dv.getUint32(4, true);
    var off = 8;
    var done = 0;
    var CHUNK = 15000;

    function buildChunk() {
      var end = Math.min(count, done + CHUNK);
      var pos = [];   // x, y(height), z(-north) triples
      var idx = [];   // triangle indices into pos / 3
      for (; done < end; done++) {
        var base = dv.getFloat32(off, true);
        var height = dv.getFloat32(off + 4, true);
        var n = dv.getUint16(off + 8, true);
        off += 10;
        var xs = new Array(n), ys = new Array(n), contour = new Array(n);
        for (var v = 0; v < n; v++) {
          var x = dv.getFloat32(off, true), y = dv.getFloat32(off + 4, true);
          off += 8;
          xs[v] = x; ys[v] = y;
          contour[v] = new THREE.Vector2(x, y);
        }
        if (n < 3) continue;
        var top = base + Math.max(height, 1.0);
        var first = pos.length / 3;

        // Roof vertices (indices first .. first+n-1) at the top height.
        for (v = 0; v < n; v++) pos.push(xs[v], top, -ys[v]);
        // Bottom vertices (indices first+n .. first+2n-1) at the base height.
        for (v = 0; v < n; v++) pos.push(xs[v], base, -ys[v]);

        var faces = THREE.ShapeUtils.triangulateShape(contour, []);
        for (var f = 0; f < faces.length; f++) {
          var t = faces[f];
          idx.push(first + t[0], first + t[1], first + t[2]);
        }
        for (var e = 0; e < n; e++) {
          var e2 = (e + 1) % n;
          var tTop0 = first + e, tTop1 = first + e2;
          var bBot0 = first + n + e, bBot1 = first + n + e2;
          idx.push(bBot0, bBot1, tTop1);
          idx.push(bBot0, tTop1, tTop0);
        }
      }
      return { pos: pos, idx: idx };
    }

    function step() {
      try {
        var chunk = buildChunk();
        if (chunk.idx.length) {
          var g = new THREE.BufferGeometry();
          g.setAttribute('position', new THREE.Float32BufferAttribute(chunk.pos, 3));
          g.setIndex(new THREE.Uint32BufferAttribute(chunk.idx, 1));
          buildingGroup.add(new THREE.Mesh(g, buildingMat));
        }
        onProgress(done, count);
        if (done < count) setTimeout(step, 0);
        else onDone(count);
      } catch (e) {
        showError(e);
      }
    }
    step();
  }

  // ---- input --------------------------------------------------------------------
  var canvas = renderer.domElement;
  var pointers = new Map();
  var lastPinch = 0;

  function dist2(a, b) { var dx = a.x - b.x, dy = a.y - b.y; return Math.sqrt(dx * dx + dy * dy); }

  canvas.addEventListener('pointerdown', function (e) {
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
  });
  canvas.addEventListener('pointermove', function (e) {
    if (!pointers.has(e.pointerId)) return;
    var prev = pointers.get(e.pointerId);
    pointers.set(e.pointerId, { x: e.clientX, y: e.clientY });
    if (pointers.size === 1) {
      view.theta -= (e.clientX - prev.x) * 0.006;
      view.phi = clamp(view.phi - (e.clientY - prev.y) * 0.005, 0.05, 1.5);
    } else if (pointers.size === 2) {
      var pts = Array.from(pointers.values());
      var d = dist2(pts[0], pts[1]);
      if (lastPinch > 0 && d > 0) view.dist = clamp(view.dist * lastPinch / d, MIN_DIST, MAX_DIST);
      lastPinch = d;
    }
    updateCamera();
  });
  function endPointer(e) {
    pointers.delete(e.pointerId);
    lastPinch = 0;
  }
  canvas.addEventListener('pointerup', endPointer);
  canvas.addEventListener('pointercancel', endPointer);
  canvas.addEventListener('pointerleave', endPointer);
  canvas.addEventListener('wheel', function (e) {
    e.preventDefault();
    view.dist = clamp(view.dist * Math.exp(e.deltaY * 0.001), MIN_DIST, MAX_DIST);
    updateCamera();
  }, { passive: false });

  document.getElementById('btnReset').addEventListener('click', function () {
    view.theta = 0; view.phi = 1.0; view.dist = 60000;
    updateCamera();
  });
  var buildingsBtn = document.getElementById('btnBuildings');
  buildingsBtn.addEventListener('click', function () {
    buildingGroup.visible = !buildingGroup.visible;
    buildingsBtn.textContent = 'Buildings: ' + (buildingGroup.visible ? 'on' : 'off');
  });

  window.addEventListener('resize', function () {
    renderer.setSize(window.innerWidth, window.innerHeight);
    camera.aspect = window.innerWidth / window.innerHeight;
    camera.updateProjectionMatrix();
  });

  // ---- startup ------------------------------------------------------------------
  function animate() {
    requestAnimationFrame(animate);
    renderer.render(scene, camera);
  }
  updateCamera();
  animate();

  Promise.all([fetchJson(DATA + 'meta.json'), fetchBuf(DATA + 'terrain.f32')])
    .then(function (res) {
      var meta = res[0];
      buildTerrain(res[1], meta);
      var extent = meta.terrain.cell_m;
      view.cx = meta.terrain.width * extent / 2;
      view.cy = meta.terrain.height * extent / 2;
      updateCamera();
      setStatus('Terrain loaded. Loading buildings...');
      return fetchBuf(DATA + 'buildings.bin').then(function (b) {
        buildBuildings(b,
          function (n, total) { setStatus('Loading buildings: ' + n + ' / ' + total); },
          function (total) {
            setStatus('Singapore: ' + total + ' building outlines. Drag to rotate, pinch or scroll to zoom.');
          });
      });
    })
    .catch(showError);
})();
