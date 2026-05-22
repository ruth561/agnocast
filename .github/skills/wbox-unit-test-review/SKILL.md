---
name: White Box Unit Test Review
description: This skill reviews the white box unit tests specified int the prompt and provides feedback on their quality.
---

# レビューする観点

- AAAパターンに従っているか
- コードと密結合していないか
- １つのテストケースは１つの機能をテストしているか
- テストケースは網羅できているか

# レポートするときには、以下のフォーマットで出力してください

```
# レビュー結果

「レビューする観点」に基づいて、各テストケースの評価を行い、結果をまとめる。

# テストケースの簡単な説明

それぞれのテストケースについて、どのような機能をテストしているのか、人間が理解しやすいように簡潔に説明する。テストの網羅性は、人間とAIのダブルチェックで行う。
```
