import 'package:flutter/material.dart';

import 'placeholder_screen.dart';

class BindingsScreen extends StatelessWidget {
  const BindingsScreen({super.key});

  @override
  Widget build(BuildContext context) => const PlaceholderScreen(
        title: 'Commands & Bindings',
        plan: [
          'Right hand — mouse control rows',
          'Left hand — keyboard control rows',
          'Per-row enable and rebind',
          'Key hold / release pairing enforced in the editor',
        ],
      );
}
