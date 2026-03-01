# WiiUStreamDeck

Turn your Wii U GamePad into a fully customizable Stream Deck-style controller.

WiiUStreamDeck is an open-source homebrew application that transforms your Wii U GamePad into a powerful macro controller for your PC. Designed for streamers, content creators, and power users, it gives a second life to your Wii U hardware.

---

## 🚀 Features

- 🎛 Customizable button layouts  
- 🎬 OBS scene switching support  
- ⌨️ Keyboard shortcuts & macro execution  
- 🚀 Application launcher  
- 🎵 Media controls  
- 🔄 Real-time button feedback  
- ⚡ Lightweight and responsive  
- 🔓 Fully open-source  

---

## 🖥️ Architecture Overview

WiiUStreamDeck works with two components:

### Wii U (Homebrew App)
- Runs directly on the Wii U via the Homebrew Launcher  
- Displays the interactive button interface  
- Communicates with the PC client over the local network  

### PC Client
- Receives input from the Wii U GamePad  
- Executes configured actions (shortcuts, OBS commands, macros, etc.)  
- Provides configuration and profile management  

---

## 📦 Installation

### Wii U Setup

1. Install the Homebrew Launcher on your Wii U  
2. Copy the `WiiUStreamDeck` folder to your SD card (`/wiiu/apps/`)  
3. Launch the app from the Homebrew Launcher  

### PC Setup

1. Download the latest release from the Releases page  
2. Launch the WiiUStreamDeck PC client  
3. Configure your buttons and actions  
4. Connect the Wii U to the same local network  

---

## 📋 Requirements

- A hacked Wii U with Homebrew Launcher installed  
- Wii U GamePad  
- Windows PC (Linux support planned)  
- Local network connection  

---

## 🛠️ Roadmap

- Plugin system  
- Custom icon upload  
- Multi-page profiles  
- Linux support  
- macOS support  
- Web-based configuration interface  
- Profile import/export  

---

## 🤝 Contributing

Contributions are welcome!

If you would like to help:
- Open an issue to report bugs or suggest features  
- Submit a pull request  
- Improve documentation  

Please make sure to follow the project structure and coding conventions.

---

## 📄 License

This project is licensed under the MIT License.  
See the `LICENSE` file for details.

---

## ⭐ Support the Project

If you like this project, consider giving it a star on GitHub.  
It helps increase visibility and motivates further development.
