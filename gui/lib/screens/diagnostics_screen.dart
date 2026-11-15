import 'package:flutter/material.dart';

import 'placeholder_screen.dart';

class DiagnosticsScreen extends StatelessWidget {
  const DiagnosticsScreen({super.key});

  @override
  Widget build(BuildContext context) => const PlaceholderScreen(
        title: 'Diagnostics',
        plan: [
          'Input backend unavailable (Wayland / XTest) — critical',
          'Low frame rate — warning, with what to change',
          'Camera and model status — healthy cards',
        ],
      );
}
