// Fills in the download buttons from the newest GitHub release (pre-releases included). Without JavaScript, or if
// GitHub cannot be reached, the buttons stay as links to the releases page.
(function () {
  "use strict";
  var API = "https://api.github.com/repos/JonathanDC64/FFCCEOT-Recomp/releases";

  function size(bytes) {
    return (bytes / (1024 * 1024)).toFixed(bytes > 99 * 1024 * 1024 ? 0 : 1) + " MB";
  }

  function setButton(id, asset, what) {
    var a = document.getElementById(id);
    if (!a || !asset) return;
    a.href = asset.browser_download_url;
    var meta = a.querySelector("[data-meta]");
    if (meta) meta.textContent = what + " · " + size(asset.size);
  }

  fetch(API + "?per_page=10", { headers: { Accept: "application/vnd.github+json" } })
    .then(function (r) { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(function (list) {
      var rel = list.filter(function (r) { return !r.draft; })[0];
      if (!rel) return;
      var win = null, linux = null;
      rel.assets.forEach(function (a) {
        if (/windows-x64\.zip$/i.test(a.name)) win = a;
        else if (/\.AppImage$/i.test(a.name)) linux = a;
      });
      setButton("dl-windows", win, "Windows 10/11, 64-bit (zip)");
      setButton("dl-linux", linux, "64-bit, glibc 2.35+ (AppImage)");
      var line = document.getElementById("release-line");
      if (line) {
        line.textContent = "";
        var link = document.createElement("a");
        link.href = rel.html_url;
        link.textContent = rel.name || rel.tag_name;
        line.appendChild(document.createTextNode("Newest: "));
        line.appendChild(link);
        line.appendChild(document.createTextNode(rel.prerelease ? " (pre-release)" : ""));
        if (rel.published_at) {
          var d = new Date(rel.published_at);
          line.appendChild(document.createTextNode(", " + d.toLocaleDateString(undefined, { year: "numeric", month: "long", day: "numeric" })));
        }
      }
    })
    .catch(function () { /* keep the fallback links */ });
})();
