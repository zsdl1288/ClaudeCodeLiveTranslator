    {
      // Synthetic events and an unstarted worker keep this regression offline.
      const auto routingSavedPayload = taskPayload();
      const auto routingSavedWorker = worker;
      auto routingSavedCache = std::move(taskCache);
      const auto routingSavedSettings = settings;
      const auto routingSavedSelected = selectedWindowToken;
      const auto routingSavedGeneration = generation;
      const auto routingSavedSourceGeneration = sourceGeneration;
      const bool routingSavedRunning = running, routingSavedPaused = paused;
      const bool routingSavedDirty = dirty, routingSavedCacheDirty = cacheDirty;
      const bool routingSavedRouting = routingWindowEvent, routingSavedSuppression = suppressTaskUi;
      const auto routingSavedCacheTime = lastCacheSave;
      const auto routingSavedStatus = textOf(statusLabel);
      const auto routingSavedPauseCaption = textOf(pauseButton);
      auto routingRestore = [&] {
        worker->cancelAll();
        worker = routingSavedWorker;
        taskCache = std::move(routingSavedCache);
        settings = routingSavedSettings;
        generation = routingSavedGeneration;
        sourceGeneration = routingSavedSourceGeneration;
        running = routingSavedRunning;
        paused = routingSavedPaused;
        routingWindowEvent = routingSavedRouting;
        suppressTaskUi = routingSavedSuppression;
        selectedWindowToken = routingSavedSelected;
        loadWindowTask(routingSavedPayload, false);
        restoreWindowView();
        SetWindowTextW(statusLabel, routingSavedStatus.c_str());
        SetWindowTextW(pauseButton, routingSavedPauseCaption.c_str());
        dirty = routingSavedDirty;
        cacheDirty = routingSavedCacheDirty;
        lastCacheSave = routingSavedCacheTime;
      };
      try {
        worker = std::make_shared<Worker>(std::make_shared<Bus>());
        taskCache = std::make_unique<cclive::TaskCache>();
        settings.provider = "demo";
        settings.target = "zh-CN";
        ++generation;
        ++sourceGeneration;
        running = true;
        paused = routingWindowEvent = suppressTaskUi = false;
        selectedWindowToken.clear();
        loadWindowTask(emptyWindowTask(json::object()), false);
        const std::string taskA = "routing-A|same-conversation", taskB = "routing-B|same-conversation";
        auto routingMeta = [&](const std::string &token, const std::string &id) {
          return json{{"type", "task"}, {"window_scoped", true},
                      {"window_token", token}, {"task_id", id},
                      {"task_title", "Window " + token}, {"identity_quality", "url"},
                      {"source_hwnd", token == "A" ? 11ULL : 22ULL},
                      {"_generation", sourceGeneration}, {"source_signatures", json::array()}};
        };
        auto routingText = [&](const std::string &token, const std::string &id,
                               const std::string &key, const std::string &source, int epoch = 0) {
          auto event = routingMeta(token, id);
          event["type"] = "text";
          event["key"] = key;
          event["text"] = source;
          event["epoch"] = epoch;
          event["stable"] = true;
          event["block_key"] = "block-" + token;
          return event;
        };
        auto routingSelect = [&](const std::string &token) {
          accept({{"type", "select_window"}, {"window_scoped", true},
                  {"window_token", token}, {"source_hwnd", token == "A" ? 11ULL : 22ULL},
                  {"_generation", sourceGeneration}});
        };
        auto routingPayload = [&](const std::string &id) {
          return id == activeTaskId ? taskPayload()
                                   : taskCache->get(id).value_or(json::object());
        };
        auto routingRecord = [&](const std::string &id, const std::string &source) {
          for (const auto &record : routingPayload(id).value("records", json::array()))
            if (record.value("source", "") == source) return record;
          return json::object();
        };
        auto routingJob = [&](const std::string &id, const std::string &source) {
          std::lock_guard<std::mutex> lock(worker->mutex);
          for (const auto &job : worker->pending)
            if (job->taskId == id && job->source == source) return job;
          return std::shared_ptr<Job>{};
        };
        auto routingFinish = [&](const std::shared_ptr<Job> &job, const std::string &translation) {
          if (!job) return;
          acceptResult({{"task_id", job->taskId}, {"generation", job->generation},
                        {"key", job->key}, {"version", job->version},
                        {"state", "done"}, {"translation", translation}});
        };
        auto routingScroll = [&] {
          POINT point{};
          SendMessageW(translationEdit, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&point));
          return point;
        };
        auto routingReadAt = [&](LONG y) {
          followingTranslation = false;
          ScrollGuard guard(internalScroll);
          POINT point{0, y};
          SendMessageW(translationEdit, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&point));
          return routingScroll();
        };
        auto routingLines = [](const std::string &prefix) {
          std::string text;
          for (int n = 0; n < 45; ++n) text += prefix + std::to_string(n) + "\n";
          return text;
        };

        routingSelect("A");
        accept(routingMeta("A", taskA));
        accept(routingText("A", taskA, "first", "A first English sentence."));
        const auto firstA = routingJob(taskA, "A first English sentence.");
        accept(routingMeta("B", taskB));
        accept(routingText("B", taskB, "first", "B first English sentence."));
        const auto firstB = routingJob(taskB, "B first English sentence.");
        check("window_scoped_same_record_keys_keep_both_pending_jobs_independent",
              firstA && firstB && firstA->key == firstB->key &&
              !firstA->cancel->load() && !firstB->cancel->load() &&
              worker->submitted.load() == 2 && activeTaskId == taskA);

        routingSelect("B");
        const auto longB = routingLines("乙窗口阅读位置 ");
        routingFinish(firstB, longB);
        render();
        const auto scrollB = routingReadAt(s(85));
        followingSource = true;
        const auto bodyB = textOf(translationEdit), titleB = textOf(taskLabel);
        const auto markerB = latestVisibleRecord() ? latestVisibleRecord()->displayedUpdate : 0;
        const auto beforeBackgroundSource = worker->submitted.load();
        accept(routingText("A", taskA, "second", "A background English sentence."));
        const auto secondA = routingJob(taskA, "A background English sentence.");
        const auto longA = routingLines("甲窗口阅读位置 ");
        routingFinish(firstA, longA);
        routingFinish(secondA, "甲窗口后台新增译文。");
        check("background_window_new_English_is_queued_and_completed_without_touching_visible_window",
              worker->submitted.load() == beforeBackgroundSource + 1 &&
              routingRecord(taskA, "A background English sentence.").value("translation", "") == "甲窗口后台新增译文。" &&
              activeTaskId == taskB && textOf(translationEdit) == bodyB && textOf(taskLabel) == titleB &&
              latestVisibleRecord() && latestVisibleRecord()->displayedUpdate == markerB &&
              !followingTranslation && abs(routingScroll().y - scrollB.y) <= 2);
        const auto beforeSelectA = worker->submitted.load();
        const auto versionsA = routingPayload(taskA).at("records");
        routingSelect("A");
        check("window_selection_restores_completed_background_results_without_resubmitting_or_revising",
              activeTaskId == taskA && worker->submitted.load() == beforeSelectA &&
              taskPayload().at("records") == versionsA &&
              textOf(translationEdit).find(L"甲窗口后台新增译文") != std::wstring::npos &&
              followingTranslation);
        followingSource = false;
        const auto scrollA = routingReadAt(s(155));
        routingSelect("B");
        const bool restoredB = !followingTranslation && followingSource && abs(routingScroll().y - scrollB.y) <= 2;
        routingSelect("A");
        check("each_window_preserves_independent_reading_position_and_follow_flags",
              restoredB && !followingTranslation && !followingSource &&
              abs(routingScroll().y - scrollA.y) <= 2 && scrollA.y > scrollB.y);

        accept(routingText("A", taskA, "revision", "A pending revision."));
        const auto oldRevisionA = routingJob(taskA, "A pending revision.");
        accept(routingText("B", taskB, "revision", "B pending revision."));
        const auto pendingB = routingJob(taskB, "B pending revision.");
        routingSelect("B");
        accept(routingText("A", taskA, "revision", "A revised pending sentence."));
        const auto revisedA = routingJob(taskA, "A revised pending sentence.");
        check("background_revision_cancels_only_matching_task_and_record",
              oldRevisionA && revisedA && pendingB && oldRevisionA->cancel->load() &&
              revisedA->version > oldRevisionA->version && !pendingB->cancel->load() && activeTaskId == taskB);
        const auto beforeResetGeneration = generation;
        auto resetA = routingMeta("A", taskA);
        resetA["type"] = "reset";
        resetA["epoch"] = 1;
        accept(resetA);
        check("one_window_ledger_reset_preserves_other_window_jobs_and_session_generation",
              pendingB && !pendingB->cancel->load() && generation == beforeResetGeneration &&
              routingPayload(taskA).at("window_runtime").value("ledger_epoch", -1) == 1 &&
              activeTaskId == taskB && revisedA && revisedA->cancel->load());

        const auto afterResetA = routingJob(taskA, "A revised pending sentence.");
        routingSelect("A");
        clear(true);
        routingSelect("B");
        render();
        const auto bodyBeforeClearedResult = textOf(translationEdit);
        routingFinish(afterResetA, "清空后后台完成的甲窗口译文。");
        const bool backgroundClearStayedInA = activeTaskId == taskB &&
            textOf(translationEdit) == bodyBeforeClearedResult;
        routingSelect("A");
        check("cleared_window_pending_result_returns_only_to_its_own_caption_cache",
              backgroundClearStayedInA && records.size() == 1 && records[0].visible &&
              records[0].translation == "清空后后台完成的甲窗口译文。" &&
              textOf(translationEdit).find(L"甲窗口阅读位置") == std::wstring::npos);

        accept(routingText("A", taskA, "pause", "A pending pause test.", 1));
        const auto pausedA = routingJob(taskA, "A pending pause test.");
        const auto staleGeneration = generation;
        pause();
        const bool pausedBoth = paused && pausedA && pendingB &&
            pausedA->cancel->load() && pendingB->cancel->load();
        pause();
        const auto resumedA = routingJob(taskA, "A pending pause test.");
        const auto resumedB = routingJob(taskB, "B pending revision.");
        check("pause_and_resume_cancel_and_resume_pending_jobs_for_all_window_caches",
              pausedBoth && !paused && resumedA && resumedB &&
              !resumedA->cancel->load() && !resumedB->cancel->load() &&
              resumedA->generation == generation && resumedB->generation == generation &&
              generation > staleGeneration);
        const auto beforeStaleResult = routingRecord(taskA, "A pending pause test.");
        routingFinish(pausedA, "过期任务不得覆盖窗口缓存。");
        check("old_session_result_cannot_overwrite_resumed_window_record",
              routingRecord(taskA, "A pending pause test.") == beforeStaleResult);

        auto oldReaderPayload = taskPayload();
        oldReaderPayload["window_runtime"]["ledger_epoch"] = 9;
        saveActiveTask();
        worker->cancelAll();
        ++generation;
        ++sourceGeneration;
        loadWindowTask(oldReaderPayload);
        const bool resetOldEpoch = ledgerEpoch == -1 && sourceAliases.empty();
        accept(routingMeta("A", taskA));
        accept(routingText("A", taskA, "fresh-reader", "New reader epoch zero sentence."));
        check("reader_restart_resets_cached_ledger_epoch_and_accepts_new_epoch_zero_text",
              resetOldEpoch && ledgerEpoch == 0 &&
              !routingRecord(taskA, "New reader epoch zero sentence.").empty());

        routingSelect("B");
        const std::string taskC = "routing-C|bounded-history";
        accept(routingMeta("C", taskC));
        for (int n = 0; n < 130; ++n)
          accept(routingText("C", taskC, "line-" + std::to_string(n),
                             "Background bounded line " + std::to_string(n) + "."));
        const auto boundedC = routingPayload(taskC).value("records", json::array());
        check("background_window_caption_cache_retains_only_latest_100_logical_lines",
              activeTaskId == taskB && boundedC.size() == 100 &&
              boundedC.front().value("source", "") == "Background bounded line 30." &&
              boundedC.back().value("source", "") == "Background bounded line 129.");
        const auto evictedJob = routingJob(taskC, "Background bounded line 129.");
        for (int n = 0; n < 52; ++n) {
          const auto token = "eviction-" + std::to_string(n);
          accept(routingMeta(token, token + "|chat"));
        }
        const auto visibleBeforeEvictedResult = textOf(translationEdit);
        const bool cacheEvicted = !taskCache->get(taskC);
        routingFinish(evictedJob, "淘汰结果不得恢复缓存。");
        check("evicted_background_cache_cancels_jobs_and_ignores_late_result",
              cacheEvicted && evictedJob && evictedJob->cancel->load() &&
              !taskCache->get(taskC) && activeTaskId == taskB &&
              textOf(translationEdit) == visibleBeforeEvictedResult && taskCache->keys().size() <= 50);

        Bus selectionOverflow;
        selectionOverflow.emit({{"type", "select_window"}, {"window_scoped", true}, {"window_token", "A"}});
        for (int n = 0; n < 512; ++n)
          selectionOverflow.emit({{"type", "text"}, {"window_scoped", true}, {"window_token", "A"}});
        check("source_overflow_retains_latest_window_selection_outside_bounded_fifo",
              selectionOverflow.source.size() == 512 && selectionOverflow.selection &&
              selectionOverflow.selection->value("window_token", "") == "A");
        selectionOverflow.emit({{"type", "select_window"}, {"window_scoped", true}, {"window_token", "B"}});
        check("new_window_selection_supersedes_overflowed_selection_without_replaying_old_focus",
              !selectionOverflow.selection && selectionOverflow.source.back().value("window_token", "") == "B" &&
              std::count_if(selectionOverflow.source.begin(), selectionOverflow.source.end(), [](const json &event) {
                return event.value("type", "") == "select_window";
              }) == 1);

        Bus isolatedResultBus;
        isolatedResultBus.result({{"task_id", "A"}, {"generation", 1},
                                  {"key", "record-1"}, {"version", 1}, {"translation", "甲"}});
        isolatedResultBus.result({{"task_id", "B"}, {"generation", 1},
                                  {"key", "record-1"}, {"version", 1}, {"translation", "乙"}});
        check("result_bus_keeps_identical_record_versions_from_different_tasks_distinct",
              isolatedResultBus.results.size() == 2);
      } catch (...) {
        routingRestore();
        throw;
      }
      routingRestore();
    }
