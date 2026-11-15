import 'package:flutter/material.dart';

import 'placeholder_screen.dart';

class SettingsScreen extends StatelessWidget {
  const SettingsScreen({super.key});

  @override
  Widget build(BuildContext context) => const PlaceholderScreen(
        title: 'Settings',
        plan: [
          'Camera device and resolution',
          'Cursor and scroll sensitivity',
          'Detection threshold, repeat while held',
          'Launch at startup, global panic hotkey',
          'Language (UZ / EN / RU)',
        ],
      );
}
