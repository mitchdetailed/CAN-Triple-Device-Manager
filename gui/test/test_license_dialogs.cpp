// The two dialogs the licensing work added, driven offscreen with no device.
//
// Neither dialog can be tested end to end here — one writes flash on a unit,
// the other writes a package that only a unit can consume — so what is pinned
// is the half that decides things before any hardware is touched: which inputs
// enable the action, which refuse it, and why. Those rules are the difference
// between a Builder that produces packages nothing can install and one that
// stops you at the desk, and between a licence dialog that lets you tick
// "remove the password" while typing a new one and one that will not.
//
// Widgets are located by what a person sees — button role, placeholder, label
// and checkbox text — and where creation order is relied on, the choice is
// cross-checked against something visible so a reordered form fails loudly
// rather than passing against the wrong field.

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton> // QDialogButtonBox::button() returns one; the base-class conversion needs the full type
#include <QSpinBox>

#include <cstdio>

#include "../src/protocol/device_link.h"
#include "../src/protocol/wire_structs.h"
#include "../src/ui/firmware_license_dialog.h"
#include "../src/ui/secure_builder_dialog.h"

static int fails = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        if (!(cond)) {                                                               \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);              \
            ++fails;                                                                 \
        }                                                                            \
    } while (0)

namespace {

using namespace ct;

QCheckBox *checkboxNamed(QWidget *root, const QString &text)
{
    for (QCheckBox *c : root->findChildren<QCheckBox *>())
        if (c->text() == text)
            return c;
    return nullptr;
}

QLabel *labelStartingWith(QWidget *root, const QString &prefix)
{
    for (QLabel *l : root->findChildren<QLabel *>())
        if (l->text().startsWith(prefix))
            return l;
    return nullptr;
}

QAbstractButton *buttonWithRole(QWidget *root, QDialogButtonBox::ButtonRole role)
{
    auto *box = root->findChild<QDialogButtonBox *>();
    if (!box)
        return nullptr;
    for (QAbstractButton *b : box->buttons())
        if (box->buttonRole(b) == role)
            return b;
    return nullptr;
}

// ------------------------------------------------------------ the Builder

void testBuilderRefusesWhatCannotInstall()
{
    // Braces, not parentheses: `dlg(QString())` is the most vexing parse and
    // declares a function taking a function pointer, silently.
    SecureBuilderDialog dlg{QString()};

    QAbstractButton *build = buttonWithRole(&dlg, QDialogButtonBox::AcceptRole);
    CHECK(build != nullptr);
    if (!build)
        return;

    // Creation order: source, three match fields, the key, six password
    // fields. Cross-checked by what is visible so a reordered form is caught.
    //
    // The Package version QSpinBox owns a QLineEdit of its own, which
    // findChildren() also returns — the first run of this test counted twelve
    // and every index past the spinbox was off by one. Exclude it by parent.
    QList<QLineEdit *> edits;
    for (QLineEdit *e : dlg.findChildren<QLineEdit *>())
        if (!(e->parent() && e->parent()->inherits("QAbstractSpinBox")))
            edits << e;
    CHECK(edits.size() == 11);
    if (edits.size() != 11)
        return;
    QLineEdit *source = edits[0];
    QLineEdit *matchModel = edits[2];
    QLineEdit *key = edits[4];
    QLineEdit *sendPassword = edits[5];
    CHECK(source->placeholderText() == QStringLiteral("Choose a .ct3 to package"));
    CHECK(key->echoMode() == QLineEdit::Password);
    CHECK(key->isEnabled()); // the one secret with no checkbox: always live
    CHECK(sendPassword->echoMode() == QLineEdit::Password);
    CHECK(!sendPassword->isEnabled()); // gated behind its checkbox
    CHECK(!matchModel->isEnabled());

    // Nothing filled in: no source, no key. Refused.
    CHECK(!build->isEnabled());

    // A source alone is not enough — the key is mandatory and has no checkbox
    // to opt out of.
    source->setText(QStringLiteral("C:/anything.ct3"));
    CHECK(!build->isEnabled());
    key->setText(QStringLiteral("fleet master phrase"));
    CHECK(build->isEnabled());

    // A ticked match with nothing typed would compare against an empty string,
    // which no licence can equal: a package nothing could ever install. Caught
    // here, named, rather than at install where it would look like a device
    // fault.
    QCheckBox *matchModelCheck = checkboxNamed(&dlg, QStringLiteral("Match FW Model:"));
    CHECK(matchModelCheck != nullptr);
    if (!matchModelCheck)
        return;
    matchModelCheck->setChecked(true);
    CHECK(matchModel->isEnabled()); // the checkbox is what enables the field
    CHECK(!build->isEnabled());
    CHECK(labelStartingWith(&dlg, QStringLiteral("Match FW Model is ticked but empty")) != nullptr);
    matchModel->setText(QStringLiteral("CAN Triple TD"));
    CHECK(build->isEnabled());
    matchModelCheck->setChecked(false);
    CHECK(!matchModel->isEnabled());
    CHECK(build->isEnabled()); // unticked: not asked for, whatever the field holds

    // A password row: ticked and EMPTY is a clear, and allowed. Ticked with a
    // weak phrase is refused under the same policy every other dialog applies —
    // a package sets that password on every unit it touches.
    QCheckBox *sendCheck = checkboxNamed(&dlg, QStringLiteral("Update Send Config Password:"));
    CHECK(sendCheck != nullptr);
    if (!sendCheck)
        return;
    sendCheck->setChecked(true);
    CHECK(sendPassword->isEnabled());
    CHECK(build->isEnabled()); // empty = clear that password
    sendPassword->setText(QStringLiteral("a"));
    CHECK(!build->isEnabled());
    CHECK(labelStartingWith(&dlg, QStringLiteral("Send Config Password:")) != nullptr);
    sendPassword->setText(QStringLiteral("a-long-enough-fleet-password"));
    CHECK(build->isEnabled());

    // The package version: the one numeric input, bounded to the wire's u16.
    auto *version = dlg.findChild<QSpinBox *>();
    CHECK(version != nullptr);
    if (version) {
        CHECK(version->minimum() == 0);
        CHECK(version->maximum() == 65535);
        CHECK(version->value() == 0); // unversioned until somebody numbers it
    }
}

// ----------------------------------------------------- the licence dialog

void testLicenseDialogOfflineAndTheClearBox()
{
    DeviceLink link;
    CHECK(!link.isOpen());
    int connectAsked = 0;
    FirmwareLicenseDialog dlg(&link, [&connectAsked]() {
        ++connectAsked;
        return false; // "the user cancelled the connection dialog"
    });

    // Opens without hardware, says so, and keeps Apply live — connecting is
    // Apply's first step, not a toll on opening the dialog.
    CHECK(labelStartingWith(&dlg, QStringLiteral("Not connected")) != nullptr);
    auto *box = dlg.findChild<QDialogButtonBox *>();
    CHECK(box != nullptr);
    if (!box)
        return;
    QAbstractButton *apply = box->button(QDialogButtonBox::Apply);
    CHECK(apply != nullptr);
    if (!apply)
        return;
    CHECK(apply->isEnabled());

    // The two secrets are the only password-echo fields, in the order the form
    // lays them out: key, then updater password.
    QList<QLineEdit *> secrets;
    for (QLineEdit *e : dlg.findChildren<QLineEdit *>())
        if (e->echoMode() == QLineEdit::Password)
            secrets << e;
    CHECK(secrets.size() == 2);
    if (secrets.size() != 2)
        return;
    QLineEdit *key = secrets[0];
    QLineEdit *updater = secrets[1];

    // Removing the password is offered only while the field is empty: "set it
    // to this" and "take it away" are contradictory, and the box unticks itself
    // as it disables so a tick made earlier cannot fire once it is out of sight.
    QCheckBox *clear = checkboxNamed(&dlg, QStringLiteral("Remove the FW Updater Password"));
    CHECK(clear != nullptr);
    if (!clear)
        return;
    CHECK(clear->isEnabled());
    clear->setChecked(true);
    CHECK(clear->isChecked());
    updater->setText(QStringLiteral("a-new-updater-password"));
    CHECK(!clear->isEnabled());
    CHECK(!clear->isChecked()); // unticked, not merely greyed
    updater->clear();
    CHECK(clear->isEnabled());
    CHECK(!clear->isChecked()); // and it does not spring back on its own

    // A weak Firmware Key is refused locally, and the warning names the field —
    // the device could not tell a weak phrase from a strong one, since all it
    // ever sees is a derivation.
    key->setText(QStringLiteral("a"));
    CHECK(!apply->isEnabled());
    CHECK(labelStartingWith(&dlg, QStringLiteral("Firmware Key:")) != nullptr);
    key->clear();
    CHECK(apply->isEnabled());

    // Apply on a closed link asks to connect, and when that is declined it stops
    // there: no prompt for a password, no write, no message box to dismiss.
    CHECK(connectAsked == 0);
    apply->click();
    CHECK(connectAsked == 1);
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    testBuilderRefusesWhatCannotInstall();
    testLicenseDialogOfflineAndTheClearBox();

    if (fails == 0)
        std::printf("test_license_dialogs: all checks passed\n");
    else
        std::printf("test_license_dialogs: %d FAILURES\n", fails);
    return fails == 0 ? 0 : 1;
}
