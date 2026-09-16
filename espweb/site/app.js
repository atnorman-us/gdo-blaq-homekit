document.querySelectorAll(".faq-question").forEach((button) => {
  button.addEventListener("click", () => {
    const answer = button.nextElementSibling;
    const expanded = button.getAttribute("aria-expanded") === "true";
    button.setAttribute("aria-expanded", String(!expanded));
    answer.hidden = expanded;
  });
});

const versionEl = document.getElementById("firmware-version");
const errorEl = document.getElementById("firmware-error");
const installButton = document.getElementById("install-button");

fetch("/firmware/manifest.json")
  .then((response) => {
    if (!response.ok) {
      throw new Error(`manifest request failed: ${response.status}`);
    }
    return response.json();
  })
  .then((manifest) => {
    versionEl.textContent = `Version ${manifest.version}`;
  })
  .catch(() => {
    versionEl.textContent = "Version unavailable";
    errorEl.hidden = false;
    installButton.hidden = true;
  });
