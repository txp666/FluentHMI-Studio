#pragma once

#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QMetaType>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

#include <initializer_list>
#include <utility>

namespace fhmi {

// One decoded message: field name -> value (qlonglong, double, QString, bool, or nested
// QVariantMap / QVariantList for JSON lines), in the order the device sent the fields, as a
// Python dict keeps them. Inserting a field it already has replaces the value in place.
// Equal messages hold the same fields, whatever their order (as Python dicts compare).
class Message
{
  public:
    using Entry = std::pair<QString, QVariant>;

    // Iterates the fields in order; key() and value() as on a QMap iterator.
    class const_iterator
    {
      public:
        explicit const_iterator(QList<Entry>::const_iterator it) : m_it(it) {}
        const QString &key() const { return m_it->first; }
        const QVariant &value() const { return m_it->second; }
        const QVariant &operator*() const { return m_it->second; }
        const_iterator &operator++()
        {
            ++m_it;
            return *this;
        }
        bool operator==(const const_iterator &other) const { return m_it == other.m_it; }
        bool operator!=(const const_iterator &other) const { return m_it != other.m_it; }

      private:
        QList<Entry>::const_iterator m_it;
    };

    Message() = default;
    Message(std::initializer_list<Entry> fields)
    {
        for (const Entry &field : fields) {
            insert(field.first, field.second);
        }
    }

    bool isEmpty() const { return m_entries.isEmpty(); }
    qsizetype size() const { return m_entries.size(); }
    qsizetype count() const { return m_entries.size(); }
    bool contains(const QString &key) const { return m_index.contains(key); }

    QVariant value(const QString &key, const QVariant &fallback = QVariant()) const
    {
        const auto at = m_index.constFind(key);
        return at == m_index.cend() ? fallback : m_entries.at(*at).second;
    }

    void insert(const QString &key, const QVariant &value)
    {
        const auto at = m_index.constFind(key);
        if (at != m_index.cend()) {
            m_entries[*at].second = value;
            return;
        }
        m_index.insert(key, m_entries.size());
        m_entries.append({key, value});
    }

    QStringList keys() const
    {
        QStringList out;
        out.reserve(m_entries.size());
        for (const Entry &entry : m_entries) {
            out.append(entry.first);
        }
        return out;
    }

    // The fields as a QVariantMap, which orders them by name.
    QVariantMap toVariantMap() const
    {
        QVariantMap out;
        for (const Entry &entry : m_entries) {
            out.insert(entry.first, entry.second);
        }
        return out;
    }

    const_iterator constBegin() const { return const_iterator(m_entries.cbegin()); }
    const_iterator constEnd() const { return const_iterator(m_entries.cend()); }
    const_iterator begin() const { return constBegin(); }
    const_iterator end() const { return constEnd(); }

    bool operator==(const Message &other) const
    {
        if (size() != other.size()) {
            return false;
        }
        for (const Entry &entry : m_entries) {
            const auto at = other.m_index.constFind(entry.first);
            if (at == other.m_index.cend() || other.m_entries.at(*at).second != entry.second) {
                return false;
            }
        }
        return true;
    }
    bool operator!=(const Message &other) const { return !(*this == other); }

  private:
    QList<Entry> m_entries;
    QHash<QString, qsizetype> m_index; // key -> position in m_entries
};

} // namespace fhmi

Q_DECLARE_METATYPE(fhmi::Message)
