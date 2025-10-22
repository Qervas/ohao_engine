# Fonts Directory

This directory contains fonts used by the OHAO Engine UI.

## FontAwesome Icons

The viewport toolbar uses **FontAwesome 6 Free** icons for a professional appearance.

### Download Instructions

1. Visit: https://fontawesome.com/download
2. Download **FontAwesome Free** (no account required)
3. Extract the downloaded ZIP file
4. Navigate to the `webfonts/` folder inside the extracted directory
5. Copy `fa-solid-900.ttf` to this directory (`assets/fonts/`)

### File Location

After downloading, you should have:
```
assets/fonts/fa-solid-900.ttf
```

### License

FontAwesome Free is licensed under:
- **Icons**: CC BY 4.0 License (https://creativecommons.org/licenses/by/4.0/)
- **Fonts**: SIL OFL 1.1 License (https://scripts.sil.org/OFL)
- **Code**: MIT License

More info: https://fontawesome.com/license/free

### Alternative: Direct Download Link

You can also download just the font file directly:
```bash
cd assets/fonts/
wget https://github.com/FortAwesome/Font-Awesome/raw/6.x/webfonts/fa-solid-900.ttf
```

Or using curl:
```bash
cd assets/fonts/
curl -L -o fa-solid-900.ttf https://github.com/FortAwesome/Font-Awesome/raw/6.x/webfonts/fa-solid-900.ttf
```

## Other Fonts

Regular UI fonts are loaded from ImGui's built-in font atlas.
