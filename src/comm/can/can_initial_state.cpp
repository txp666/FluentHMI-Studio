#include "comm/can/can_initial_state.h"

#include <QtCore/QCoreApplication>

#include <algorithm>
#include <limits>

namespace fhmi {
namespace {

QString tr(const char *text) { return QCoreApplication::translate("CanInitialState", text); }

bool fail(QString *error, const QString &text)
{
    if (error) {
        *error = text;
    }
    return false;
}

bool sameEncoding(const CanSignalDef &a, const CanSignalDef &b)
{
    return a.length == b.length && a.isSigned == b.isSigned && a.valueType == b.valueType && a.factor == b.factor &&
           a.offset == b.offset && a.unit == b.unit;
}

} // namespace

std::optional<CanInitialState> CanInitialState::build(const CanSignalSet &signalSet, const QString &mapping,
                                                      QString *error)
{
    CanInitialState result;
    const QStringList lines = mapping.split(QLatin1Char('\n'));
    for (int line = 0; line < lines.size(); ++line) {
        const QString text = lines.at(line).trimmed();
        if (text.isEmpty() || text.startsWith(QLatin1Char('#')) || text.startsWith(QStringLiteral("//"))) {
            continue;
        }
        const auto invalid = [&](const QString &reason) -> std::optional<CanInitialState> {
            fail(error, tr("发送初值映射第 %1 行无效：%2").arg(line + 1).arg(reason));
            return std::nullopt;
        };
        const QStringList names = text.split(QLatin1Char('='));
        if (names.size() != 2 || names.at(0).trimmed().isEmpty() || names.at(1).trimmed().isEmpty()) {
            return invalid(tr("应为 目标报文=反馈报文，例如 HMI=IO"));
        }
        const auto splitReference = [](const QString &reference) {
            const int dot = reference.lastIndexOf(QLatin1Char('.'));
            return dot < 0 ? qMakePair(reference.trimmed(), QString())
                           : qMakePair(reference.left(dot).trimmed(), reference.mid(dot + 1).trimmed());
        };
        const auto outputRef = splitReference(names.at(0));
        const auto inputRef = splitReference(names.at(1));
        if (outputRef.second.isEmpty() != inputRef.second.isEmpty() ||
            (names.at(0).contains(QLatin1Char('.')) && outputRef.second.isEmpty()) ||
            (names.at(1).contains(QLatin1Char('.')) && inputRef.second.isEmpty())) {
            return invalid(tr("两侧都应为报文名，或都应为报文名.信号名"));
        }
        QString why;
        const auto target = signalSet.target(outputRef.first, &why);
        if (!target) {
            return invalid(why);
        }
        const auto source = signalSet.target(inputRef.first, &why);
        if (!source) {
            return invalid(why);
        }
        if (!target->hasSignals || !source->hasSignals) {
            return invalid(tr("目标和来源均须有 DBC 或内联信号定义"));
        }
        Rule &rule = result.m_rules[target->id.key()];
        if (rule.targetName.isEmpty()) {
            rule.targetName = outputRef.first;
            rule.usedBits = QByteArray(target->length, '\0');
        }
        const auto addBinding = [&](const CanSignalDef &output, const CanSignalDef &input) {
            if (!sameEncoding(output, input) || !output.fits(target->length) || !input.fits(source->length)) {
                why = tr("映射信号 %1 与 %2 的编码不一致或超出报文长度").arg(output.name, input.name);
                return false;
            }
            QByteArray bits(target->length, '\0');
            output.putRawBits(bits, std::numeric_limits<quint64>::max());
            for (int at = 0; at < bits.size(); ++at) {
                if ((quint8(rule.usedBits.at(at)) & quint8(bits.at(at))) != 0) {
                    why = tr("目标信号 %1 与其他映射信号重叠").arg(output.name);
                    return false;
                }
                rule.usedBits[at] = char(quint8(rule.usedBits.at(at)) | quint8(bits.at(at)));
            }
            rule.bindings.append({output, input, source->id.key()});
            Source &entry = result.m_sources[source->id.key()];
            entry.id = source->id;
            entry.name = inputRef.first;
            if (!source->name.isEmpty() || source->fd) {
                entry.fd = source->fd;
            }
            entry.length = std::max(entry.length, input.bytesNeeded());
            return true;
        };
        if (!outputRef.second.isEmpty()) {
            const auto find = [](const auto &definitions, const QString &name) -> const CanSignalDef * {
                for (const CanSignalDef *definition : definitions) {
                    if (definition->name == name) {
                        return definition;
                    }
                }
                return nullptr;
            };
            const auto *output = find(target->signalDefs, outputRef.second);
            const auto *input = find(source->signalDefs, inputRef.second);
            if (!output || !input) {
                return invalid(tr("映射的信号不存在，请核对报文名和信号名"));
            }
            if (!addBinding(*output, *input)) {
                return invalid(why);
            }
        } else {
            const int previous = rule.bindings.size();
            for (const CanSignalDef *output : target->signalDefs) {
                for (const CanSignalDef *input : source->signalDefs) {
                    if (input->name == output->name && !addBinding(*output, *input)) {
                        return invalid(why);
                    }
                }
            }
            if (rule.bindings.size() == previous) {
                return invalid(tr("两个报文没有编码一致的同名信号"));
            }
        }
    }
    return result;
}

std::optional<QString> CanInitialState::migrateSignalMapping(const CanSignalSet &before, const CanSignalSet &after,
                                                              const QString &mapping,
                                                              const QHash<QString, QString> &changes)
{
    bool affected = false;
    auto lines = mapping.split(u'\n');
    for (auto &line : lines) {
        const auto original = build(before, line, nullptr);
        if (!original || original->m_rules.isEmpty())
            continue;
        const auto &rule = original->m_rules.cbegin().value();
        const auto &source = original->m_sources.cbegin().value();
        const auto target = before.target(rule.targetName);
        const auto input = before.target(source.name);
        const auto changed = [&](const CanId &id, const CanSignalDef *signal) {
            return changes.contains(inlineSignalKey(id, signal->name));
        };
        if (std::none_of(target->signalDefs.cbegin(), target->signalDefs.cend(), [&](auto *signal) {
                return changed(target->id, signal);
            }) && std::none_of(input->signalDefs.cbegin(), input->signalDefs.cend(), [&](auto *signal) {
                return changed(source.id, signal);
            }))
            continue;

        QStringList explicitPairs;
        QHash<QString, QString> expected;
        bool changedPair = false;
        for (const auto &pair : rule.bindings) {
            const auto outputKey = inlineSignalKey(target->id, pair.target.name);
            const auto inputKey = inlineSignalKey(source.id, pair.source.name);
            const auto outputName = changes.value(outputKey, pair.target.name);
            const auto inputName = changes.value(inputKey, pair.source.name);
            changedPair |= changes.contains(outputKey) || changes.contains(inputKey);
            expected.insert(outputName, inputName);
            explicitPairs.append(QStringLiteral("%1.%2=%3.%4").arg(rule.targetName, outputName, source.name, inputName));
        }
        const auto updated = build(after, line, nullptr);
        QHash<QString, QString> actual;
        if (updated)
            for (const auto &pair : updated->m_rules.cbegin()->bindings)
                actual.insert(pair.target.name, pair.source.name);
        if (actual != expected)
            line = explicitPairs.join(u'\n');
        affected |= changedPair || actual != expected;
    }
    return affected ? std::optional<QString>(lines.join(u'\n')) : std::nullopt;
}

bool CanInitialState::acceptsFilter(const CanFilter &filter, QString *error) const
{
    for (const Source &source : m_sources) {
        if (!filter.accepts(source.id.id, source.id.extended)) {
            return fail(error, tr("发送初值来源 %1 被接收过滤器排除，请先允许接收此报文").arg(source.name));
        }
    }
    return true;
}

void CanInitialState::observe(const CanFrame &frame, qint64 receivedAtMs)
{
    auto found = m_sources.find(CanId{frame.id, frame.extended}.key());
    if (found == m_sources.end() || receivedAtMs < 0 || frame.error || frame.rtr ||
        (found->fd && frame.fd != *found->fd) ||
        frame.data.size() < found->length ||
        (frame.fd ? !Can::isValidFdLength(frame.data.size()) : frame.data.size() > 8)) {
        return;
    }
    found->received = true;
    found->data = frame.data;
    found->receivedAt = receivedAtMs;
}

void CanInitialState::clearReceived()
{
    for (Source &source : m_sources) {
        source.received = false;
        source.data.clear();
    }
}

bool CanInitialState::initialize(const CanId &target, QByteArray &data, qint64 nowMs, int maxAgeMs,
                                 QString *error) const
{
    const auto rule = m_rules.constFind(target.key());
    if (rule == m_rules.constEnd()) {
        return true;
    }
    // Validate every source before writing any field, so a missing/stale source cannot partially initialize.
    for (const Binding &binding : rule->bindings) {
        const Source &source = m_sources.constFind(binding.sourceKey).value();
        if (!source.received) {
            return fail(error,
                        tr("尚未收到 %1 的完整状态，不能初始化 %2，请先接收反馈").arg(source.name, rule->targetName));
        }
        if (maxAgeMs <= 0 || nowMs < source.receivedAt || nowMs - source.receivedAt > maxAgeMs) {
            return fail(error, tr("%1 的反馈已过期，不能初始化 %2，请等待新反馈").arg(source.name, rule->targetName));
        }
        if (!binding.target.fits(data.size())) {
            return fail(error, tr("发送初值目标报文长度不匹配"));
        }
    }
    for (const Binding &binding : rule->bindings) {
        const Source &source = m_sources.constFind(binding.sourceKey).value();
        binding.target.putRawBits(data, binding.source.rawBits(source.data));
    }
    return true;
}

} // namespace fhmi
