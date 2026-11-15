import 'package:flutter/material.dart';

import '../app.dart';
import '../theme/tokens.dart';
import '../widgets/common.dart';

/// A screen that exists in the nav but is not built yet.
///
/// Deliberately says so instead of showing an empty page: a blank screen
/// reads as a bug, and anyone reviewing this branch should be able to tell
/// unbuilt from broken at a glance.
class PlaceholderScreen extends StatelessWidget {
  const PlaceholderScreen({
    super.key,
    required this.title,
    required this.plan,
  });

  final String title;

  /// What this screen will contain, in the order it will appear.
  final List<String> plan;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.all(HcSpace.xxl),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            ScreenHeader(title: title, subtitle: 'Not built yet'),
            HcPanel(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const HcLabel('PLANNED CONTENT'),
                  const SizedBox(height: HcSpace.md),
                  for (final item in plan)
                    Padding(
                      padding: const EdgeInsets.symmetric(vertical: 3),
                      child: Row(
                        children: [
                          const Icon(Icons.circle,
                              size: 4, color: HcColors.textFaint),
                          const SizedBox(width: HcSpace.md),
                          Text(item,
                              style: Theme.of(context).textTheme.bodySmall),
                        ],
                      ),
                    ),
                ],
              ),
            ),
          ],
        ),
      );
}
