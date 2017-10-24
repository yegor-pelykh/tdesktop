/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2017 John Preston, https://desktop.telegram.org
*/
#include "data/data_peer.h"

#include <rpl/filter.h>
#include <rpl/map.h>
#include "data/data_peer_values.h"
#include "lang/lang_keys.h"
#include "observer_peer.h"
#include "mainwidget.h"
#include "apiwrap.h"
#include "boxes/confirm_box.h"
#include "styles/style_history.h"
#include "auth_session.h"
#include "messenger.h"
#include "mainwindow.h"
#include "window/window_controller.h"

namespace {

constexpr auto kUpdateFullPeerTimeout = TimeMs(5000); // Not more than once in 5 seconds.

int peerColorIndex(const PeerId &peer) {
	auto myId = Auth().userId();
	auto peerId = peerToBareInt(peer);
	auto both = (QByteArray::number(peerId) + QByteArray::number(myId)).mid(0, 15);
	uchar md5[16];
	hashMd5(both.constData(), both.size(), md5);
	return (md5[peerId & 0x0F] & (peerIsUser(peer) ? 0x07 : 0x03));
}

ImagePtr generateUserpicImage(const style::icon &icon) {
	auto data = QImage(icon.size() * cIntRetinaFactor(), QImage::Format_ARGB32_Premultiplied);
	data.setDevicePixelRatio(cRetinaFactor());
	{
		Painter p(&data);
		icon.paint(p, 0, 0, icon.width());
	}
	return ImagePtr(App::pixmapFromImageInPlace(std::move(data)), "PNG");
}

} // namespace

style::color peerUserpicColor(int index) {
	static style::color peerColors[kUserColorsCount] = {
		st::historyPeer1UserpicBg,
		st::historyPeer2UserpicBg,
		st::historyPeer3UserpicBg,
		st::historyPeer4UserpicBg,
		st::historyPeer5UserpicBg,
		st::historyPeer6UserpicBg,
		st::historyPeer7UserpicBg,
		st::historyPeer8UserpicBg,
	};
	return peerColors[index];
}

class EmptyUserpic::Impl {
public:
	Impl(int index, const QString &name) : _color(peerUserpicColor(index)) {
		fillString(name);
	}

	void paint(Painter &p, int x, int y, int size);
	void paintRounded(Painter &p, int x, int y, int size);
	void paintSquare(Painter &p, int x, int y, int size);
	StorageKey uniqueKey() const;

private:
	template <typename PaintBackground>
	void paint(Painter &p, int x, int y, int size, PaintBackground paintBackground);

	void fillString(const QString &name);

	style::color _color;
	QString _string;

};

template <typename PaintBackground>
void EmptyUserpic::Impl::paint(Painter &p, int x, int y, int size, PaintBackground paintBackground) {
	auto fontsize = (size * 13) / 33;
	auto font = st::historyPeerUserpicFont->f;
	font.setPixelSize(fontsize);

	PainterHighQualityEnabler hq(p);
	p.setBrush(_color);
	p.setPen(Qt::NoPen);
	paintBackground();

	p.setFont(font);
	p.setBrush(Qt::NoBrush);
	p.setPen(st::historyPeerUserpicFg);
	p.drawText(QRect(x, y, size, size), _string, QTextOption(style::al_center));
}

void EmptyUserpic::Impl::paint(Painter &p, int x, int y, int size) {
	paint(p, x, y, size, [&p, x, y, size] {
		p.drawEllipse(x, y, size, size);
	});
}

void EmptyUserpic::Impl::paintRounded(Painter &p, int x, int y, int size) {
	paint(p, x, y, size, [&p, x, y, size] {
		p.drawRoundedRect(x, y, size, size, st::buttonRadius, st::buttonRadius);
	});
}

void EmptyUserpic::Impl::paintSquare(Painter &p, int x, int y, int size) {
	paint(p, x, y, size, [&p, x, y, size] {
		p.fillRect(x, y, size, size, p.brush());
	});
}

StorageKey EmptyUserpic::Impl::uniqueKey() const {
	auto first = 0xFFFFFFFF00000000ULL | anim::getPremultiplied(_color->c);
	auto second = uint64(0);
	memcpy(&second, _string.constData(), qMin(sizeof(second), _string.size() * sizeof(QChar)));
	return StorageKey(first, second);
}

void EmptyUserpic::Impl::fillString(const QString &name) {
	QList<QString> letters;
	QList<int> levels;
	auto level = 0;
	auto letterFound = false;
	auto ch = name.constData(), end = ch + name.size();
	while (ch != end) {
		auto emojiLength = 0;
		if (auto emoji = Ui::Emoji::Find(ch, end, &emojiLength)) {
			ch += emojiLength;
		} else if (ch->isHighSurrogate()) {
			++ch;
			if (ch != end && ch->isLowSurrogate()) {
				++ch;
			}
		} else if (!letterFound && ch->isLetterOrNumber()) {
			letterFound = true;
			if (ch + 1 != end && chIsDiac(*(ch + 1))) {
				letters.push_back(QString(ch, 2));
				levels.push_back(level);
				++ch;
			} else {
				letters.push_back(QString(ch, 1));
				levels.push_back(level);
			}
			++ch;
		} else {
			if (*ch == ' ') {
				level = 0;
				letterFound = false;
			} else if (letterFound && *ch == '-') {
				level = 1;
				letterFound = true;
			}
			++ch;
		}
	}

	// We prefer the second letter to be after ' ', but it can also be after '-'.
	_string = QString();
	if (!letters.isEmpty()) {
		_string += letters.front();
		auto bestIndex = 0;
		auto bestLevel = 2;
		for (auto i = letters.size(); i != 1;) {
			if (levels[--i] < bestLevel) {
				bestIndex = i;
				bestLevel = levels[i];
			}
		}
		if (bestIndex > 0) {
			_string += letters[bestIndex];
		}
	}
	_string = _string.toUpper();
}

EmptyUserpic::EmptyUserpic() = default;

EmptyUserpic::EmptyUserpic(int index, const QString &name) : _impl(std::make_unique<Impl>(index, name)) {
}

void EmptyUserpic::set(int index, const QString &name) {
	_impl = std::make_unique<Impl>(index, name);
}

void EmptyUserpic::clear() {
	_impl.reset();
}

void EmptyUserpic::paint(Painter &p, int x, int y, int outerWidth, int size) const {
	Expects(_impl != nullptr);
	_impl->paint(p, rtl() ? (outerWidth - x - size) : x, y, size);
}

void EmptyUserpic::paintRounded(Painter &p, int x, int y, int outerWidth, int size) const {
	Expects(_impl != nullptr);
	_impl->paintRounded(p, rtl() ? (outerWidth - x - size) : x, y, size);
}

void EmptyUserpic::paintSquare(Painter &p, int x, int y, int outerWidth, int size) const {
	Expects(_impl != nullptr);
	_impl->paintSquare(p, rtl() ? (outerWidth - x - size) : x, y, size);
}

StorageKey EmptyUserpic::uniqueKey() const {
	Expects(_impl != nullptr);
	return _impl->uniqueKey();
}

QPixmap EmptyUserpic::generate(int size) {
	auto result = QImage(QSize(size, size) * cIntRetinaFactor(), QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(cRetinaFactor());
	result.fill(Qt::transparent);
	{
		Painter p(&result);
		paint(p, 0, 0, size, size);
	}
	return App::pixmapFromImageInPlace(std::move(result));
}

EmptyUserpic::~EmptyUserpic() = default;

using UpdateFlag = Notify::PeerUpdate::Flag;

NotifySettings globalNotifyAll, globalNotifyUsers, globalNotifyChats;
NotifySettingsPtr globalNotifyAllPtr = UnknownNotifySettings, globalNotifyUsersPtr = UnknownNotifySettings, globalNotifyChatsPtr = UnknownNotifySettings;

PeerClickHandler::PeerClickHandler(not_null<PeerData*> peer) : _peer(peer) {
}

void PeerClickHandler::onClick(Qt::MouseButton button) const {
	if (button == Qt::LeftButton && App::main()) {
		if (_peer && _peer->isChannel() && App::main()->historyPeer() != _peer) {
			if (!_peer->asChannel()->isPublic() && !_peer->asChannel()->amIn()) {
				Ui::show(Box<InformBox>(lang((_peer->isMegagroup()) ? lng_group_not_accessible : lng_channel_not_accessible)));
			} else {
				App::wnd()->controller()->showPeerHistory(
					_peer,
					Window::SectionShow::Way::Forward);
			}
		} else {
			Ui::showPeerProfile(_peer);
		}
	}
}

PeerData::PeerData(const PeerId &id) : id(id), _colorIndex(peerColorIndex(id)) {
	nameText.setText(st::msgNameStyle, QString(), _textNameOptions);
	_userpicEmpty.set(_colorIndex, QString());
}

void PeerData::updateNameDelayed(const QString &newName, const QString &newNameOrPhone, const QString &newUsername) {
	if (name == newName) {
		if (isUser()) {
			if (asUser()->nameOrPhone == newNameOrPhone && asUser()->username == newUsername) {
				return;
			}
		} else if (isChannel()) {
			if (asChannel()->username == newUsername) {
				return;
			}
		} else if (isChat()) {
			return;
		}
	}

	++nameVersion;
	name = newName;
	nameText.setText(st::msgNameStyle, name, _textNameOptions);
	if (!_userpic) {
		_userpicEmpty.set(_colorIndex, name);
	}

	Notify::PeerUpdate update(this);
	update.flags |= UpdateFlag::NameChanged;
	update.oldNameFirstChars = nameFirstChars();

	if (isUser()) {
		if (asUser()->username != newUsername) {
			asUser()->username = newUsername;
			update.flags |= UpdateFlag::UsernameChanged;
		}
		asUser()->setNameOrPhone(newNameOrPhone);
	} else if (isChannel()) {
		if (asChannel()->username != newUsername) {
			asChannel()->username = newUsername;
			if (newUsername.isEmpty()) {
				asChannel()->removeFlags(
					MTPDchannel::Flag::f_username);
			} else {
				asChannel()->addFlags(MTPDchannel::Flag::f_username);
			}
			update.flags |= UpdateFlag::UsernameChanged;
		}
	}
	fillNames();
	Notify::PeerUpdated().notify(update, true);
}

ClickHandlerPtr PeerData::createOpenLink() {
	return MakeShared<PeerClickHandler>(this);
}

void PeerData::setUserpic(ImagePtr userpic) {
	_userpic = userpic;
	if (!_userpic || !_userpic->loaded()) {
		_userpicEmpty.set(_colorIndex, name);
	} else {
		_userpicEmpty.clear();
	}
}

ImagePtr PeerData::currentUserpic() const {
	if (_userpic) {
		_userpic->load();
		if (_userpic->loaded()) {
			_userpicEmpty.clear();
			return _userpic;
		}
	}
	return ImagePtr();
}

void PeerData::paintUserpic(Painter &p, int x, int y, int size) const {
	if (auto userpic = currentUserpic()) {
		p.drawPixmap(x, y, userpic->pixCircled(size, size));
	} else {
		_userpicEmpty.paint(p, x, y, x + size + x, size);
	}
}

void PeerData::paintUserpicRounded(Painter &p, int x, int y, int size) const {
	if (auto userpic = currentUserpic()) {
		p.drawPixmap(x, y, userpic->pixRounded(size, size, ImageRoundRadius::Small));
	} else {
		_userpicEmpty.paintRounded(p, x, y, x + size + x, size);
	}
}

void PeerData::paintUserpicSquare(Painter &p, int x, int y, int size) const {
	if (auto userpic = currentUserpic()) {
		p.drawPixmap(x, y, userpic->pix(size, size));
	} else {
		_userpicEmpty.paintSquare(p, x, y, x + size + x, size);
	}
}

StorageKey PeerData::userpicUniqueKey() const {
	if (photoLoc.isNull() || !_userpic || !_userpic->loaded()) {
		return _userpicEmpty.uniqueKey();
	}
	return storageKey(photoLoc);
}

void PeerData::saveUserpic(const QString &path, int size) const {
	genUserpic(size).save(path, "PNG");
}

void PeerData::saveUserpicRounded(const QString &path, int size) const {
	genUserpicRounded(size).save(path, "PNG");
}

QPixmap PeerData::genUserpic(int size) const {
	if (auto userpic = currentUserpic()) {
		return userpic->pixCircled(size, size);
	}
	auto result = QImage(QSize(size, size) * cIntRetinaFactor(), QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(cRetinaFactor());
	result.fill(Qt::transparent);
	{
		Painter p(&result);
		paintUserpic(p, 0, 0, size);
	}
	return App::pixmapFromImageInPlace(std::move(result));
}

QPixmap PeerData::genUserpicRounded(int size) const {
	if (auto userpic = currentUserpic()) {
		return userpic->pixRounded(size, size, ImageRoundRadius::Small);
	}
	auto result = QImage(QSize(size, size) * cIntRetinaFactor(), QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(cRetinaFactor());
	result.fill(Qt::transparent);
	{
		Painter p(&result);
		paintUserpicRounded(p, 0, 0, size);
	}
	return App::pixmapFromImageInPlace(std::move(result));
}

const Text &BotCommand::descriptionText() const {
	if (_descriptionText.isEmpty() && !_description.isEmpty()) {
		_descriptionText.setText(st::defaultTextStyle, _description, _textNameOptions);
	}
	return _descriptionText;
}

bool UserData::canShareThisContact() const {
	return canShareThisContactFast() || !App::phoneFromSharedContact(peerToUser(id)).isEmpty();
}

void UserData::setPhoto(const MTPUserProfilePhoto &p) { // see Local::readPeer as well
	PhotoId newPhotoId = photoId;
	ImagePtr newPhoto = _userpic;
	StorageImageLocation newPhotoLoc = photoLoc;
	switch (p.type()) {
	case mtpc_userProfilePhoto: {
		const auto &d(p.c_userProfilePhoto());
		newPhotoId = d.vphoto_id.v;
		newPhotoLoc = App::imageLocation(160, 160, d.vphoto_small);
		newPhoto = newPhotoLoc.isNull() ? ImagePtr() : ImagePtr(newPhotoLoc);
		//App::feedPhoto(App::photoFromUserPhoto(peerToUser(id), MTP_int(unixtime()), p));
	} break;
	default: {
		newPhotoId = 0;
		if (id == ServiceUserId) {
			if (!_userpic) {
				newPhoto = ImagePtr(App::pixmapFromImageInPlace(Messenger::Instance().logoNoMargin().scaledToWidth(160, Qt::SmoothTransformation)), "PNG");
			}
		} else {
			newPhoto = ImagePtr();
		}
		newPhotoLoc = StorageImageLocation();
	} break;
	}
	if (newPhotoId != photoId || newPhoto.v() != _userpic.v() || newPhotoLoc != photoLoc) {
		photoId = newPhotoId;
		setUserpic(newPhoto);
		photoLoc = newPhotoLoc;
		Notify::peerUpdatedDelayed(this, UpdateFlag::PhotoChanged);
	}
}

void PeerData::fillNames() {
	_nameWords.clear();
	_nameFirstChars.clear();
	auto toIndex = TextUtilities::RemoveAccents(name);
	if (cRussianLetters().match(toIndex).hasMatch()) {
		toIndex += ' ' + translitRusEng(toIndex);
	}
	if (isUser()) {
		if (!asUser()->nameOrPhone.isEmpty() && asUser()->nameOrPhone != name) toIndex += ' ' + TextUtilities::RemoveAccents(asUser()->nameOrPhone);
		if (!asUser()->username.isEmpty()) toIndex += ' ' + TextUtilities::RemoveAccents(asUser()->username);
	} else if (isChannel()) {
		if (!asChannel()->username.isEmpty()) toIndex += ' ' + TextUtilities::RemoveAccents(asChannel()->username);
	}
	toIndex += ' ' + rusKeyboardLayoutSwitch(toIndex);

	auto namesList = TextUtilities::PrepareSearchWords(toIndex);
	for (auto &name : namesList) {
		_nameWords.insert(name);
		_nameFirstChars.insert(name[0]);
	}
}

bool UserData::setAbout(const QString &newAbout) {
	if (_about == newAbout) {
		return false;
	}
	_about = newAbout;
	Notify::peerUpdatedDelayed(this, UpdateFlag::AboutChanged);
	return true;
}

void UserData::setRestrictionReason(const QString &text) {
	if (_restrictionReason != text) {
		_restrictionReason = text;
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::RestrictionReasonChanged);
	}
}

void UserData::setCommonChatsCount(int count) {
	if (_commonChatsCount != count) {
		_commonChatsCount = count;
		Notify::peerUpdatedDelayed(this, UpdateFlag::UserCommonChatsChanged);
	}
}

void UserData::setName(const QString &newFirstName, const QString &newLastName, const QString &newPhoneName, const QString &newUsername) {
	bool changeName = !newFirstName.isEmpty() || !newLastName.isEmpty();

	QString newFullName;
	if (changeName && newFirstName.trimmed().isEmpty()) {
		firstName = newLastName;
		lastName = QString();
		newFullName = firstName;
	} else {
		if (changeName) {
			firstName = newFirstName;
			lastName = newLastName;
		}
		newFullName = lastName.isEmpty() ? firstName : lng_full_name(lt_first_name, firstName, lt_last_name, lastName);
	}
	updateNameDelayed(newFullName, newPhoneName, newUsername);
}

void UserData::setPhone(const QString &newPhone) {
	_phone = newPhone;
}

void UserData::setBotInfoVersion(int version) {
	if (version < 0) {
		if (botInfo) {
			if (!botInfo->commands.isEmpty()) {
				botInfo->commands.clear();
				Notify::botCommandsChanged(this);
			}
			botInfo = nullptr;
			Notify::userIsBotChanged(this);
		}
	} else if (!botInfo) {
		botInfo = std::make_unique<BotInfo>();
		botInfo->version = version;
		Notify::userIsBotChanged(this);
	} else if (botInfo->version < version) {
		if (!botInfo->commands.isEmpty()) {
			botInfo->commands.clear();
			Notify::botCommandsChanged(this);
		}
		botInfo->description.clear();
		botInfo->version = version;
		botInfo->inited = false;
	}
}

void UserData::setBotInfo(const MTPBotInfo &info) {
	switch (info.type()) {
	case mtpc_botInfo: {
		const auto &d(info.c_botInfo());
		if (peerFromUser(d.vuser_id.v) != id || !botInfo) return;

		QString desc = qs(d.vdescription);
		if (botInfo->description != desc) {
			botInfo->description = desc;
			botInfo->text = Text(st::msgMinWidth);
		}

		auto &v = d.vcommands.v;
		botInfo->commands.reserve(v.size());
		auto changedCommands = false;
		int32 j = 0;
		for (int32 i = 0, l = v.size(); i < l; ++i) {
			if (v.at(i).type() != mtpc_botCommand) continue;

			QString cmd = qs(v.at(i).c_botCommand().vcommand), desc = qs(v.at(i).c_botCommand().vdescription);
			if (botInfo->commands.size() <= j) {
				botInfo->commands.push_back(BotCommand(cmd, desc));
				changedCommands = true;
			} else {
				if (botInfo->commands[j].command != cmd) {
					botInfo->commands[j].command = cmd;
					changedCommands = true;
				}
				if (botInfo->commands[j].setDescription(desc)) {
					changedCommands = true;
				}
			}
			++j;
		}
		while (j < botInfo->commands.size()) {
			botInfo->commands.pop_back();
			changedCommands = true;
		}

		botInfo->inited = true;

		if (changedCommands) {
			Notify::botCommandsChanged(this);
		}
	} break;
	}
}

void UserData::setNameOrPhone(const QString &newNameOrPhone) {
	if (nameOrPhone != newNameOrPhone) {
		nameOrPhone = newNameOrPhone;
		phoneText.setText(st::msgNameStyle, nameOrPhone, _textNameOptions);
	}
}

void UserData::madeAction(TimeId when) {
	if (botInfo || isServiceUser(id) || when <= 0) return;

	if (onlineTill <= 0 && -onlineTill < when) {
		onlineTill = -when - SetOnlineAfterActivity;
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::UserOnlineChanged);
	} else if (onlineTill > 0 && onlineTill < when + 1) {
		onlineTill = when + SetOnlineAfterActivity;
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::UserOnlineChanged);
	}
}

void UserData::setAccessHash(uint64 accessHash) {
	if (accessHash == kInaccessibleAccessHashOld) {
		_accessHash = 0;
//		_flags.add(MTPDuser_ClientFlag::f_inaccessible | 0);
		_flags.add(MTPDuser::Flag::f_deleted);
	} else {
		_accessHash = accessHash;
	}
}

void UserData::setBlockStatus(BlockStatus blockStatus) {
	if (blockStatus != _blockStatus) {
		_blockStatus = blockStatus;
		Notify::peerUpdatedDelayed(this, UpdateFlag::UserIsBlocked);
	}
}

void UserData::setCallsStatus(CallsStatus callsStatus) {
	if (callsStatus != _callsStatus) {
		_callsStatus = callsStatus;
		Notify::peerUpdatedDelayed(this, UpdateFlag::UserHasCalls);
	}
}

bool UserData::hasCalls() const {
	return (callsStatus() != CallsStatus::Disabled) && (callsStatus() != CallsStatus::Unknown);
}

void ChatData::setPhoto(const MTPChatPhoto &p, const PhotoId &phId) { // see Local::readPeer as well
	PhotoId newPhotoId = photoId;
	ImagePtr newPhoto = _userpic;
	StorageImageLocation newPhotoLoc = photoLoc;
	switch (p.type()) {
	case mtpc_chatPhoto: {
		const auto &d(p.c_chatPhoto());
		if (phId != UnknownPeerPhotoId) {
			newPhotoId = phId;
		}
		newPhotoLoc = App::imageLocation(160, 160, d.vphoto_small);
		newPhoto = newPhotoLoc.isNull() ? ImagePtr() : ImagePtr(newPhotoLoc);
//		photoFull = newPhoto ? ImagePtr(640, 640, d.vphoto_big, ImagePtr()) : ImagePtr();
	} break;
	default: {
		newPhotoId = 0;
		newPhotoLoc = StorageImageLocation();
		newPhoto = ImagePtr();
//		photoFull = ImagePtr();
	} break;
	}
	if (newPhotoId != photoId || newPhoto.v() != _userpic.v() || newPhotoLoc != photoLoc) {
		photoId = newPhotoId;
		setUserpic(newPhoto);
		photoLoc = newPhotoLoc;
		Notify::peerUpdatedDelayed(this, UpdateFlag::PhotoChanged);
	}
}

void ChatData::setName(const QString &newName) {
	updateNameDelayed(newName.isEmpty() ? name : newName, QString(), QString());
}

void ChatData::invalidateParticipants() {
	auto wasCanEdit = canEdit();
	participants.clear();
	admins.clear();
	removeFlags(MTPDchat::Flag::f_admin);
	invitedByMe.clear();
	botStatus = 0;
	if (wasCanEdit != canEdit()) {
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::ChatCanEdit);
	}
	Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::MembersChanged | Notify::PeerUpdate::Flag::AdminsChanged);
}

void ChatData::setInviteLink(const QString &newInviteLink) {
	if (newInviteLink != _inviteLink) {
		_inviteLink = newInviteLink;
		Notify::peerUpdatedDelayed(this, UpdateFlag::InviteLinkChanged);
	}
}

ChannelData::ChannelData(const PeerId &id)
: PeerData(id)
, inputChannel(MTP_inputChannel(MTP_int(bareId()), MTP_long(0))) {
	Data::PeerFlagValue(this, MTPDchannel::Flag::f_megagroup)
		| rpl::start_with_next([this](bool megagroup) {
			if (megagroup) {
				if (!mgInfo) {
					mgInfo = std::make_unique<MegagroupInfo>();
				}
			} else if (mgInfo) {
				mgInfo = nullptr;
			}
		}, _lifetime);
}

void ChannelData::setPhoto(const MTPChatPhoto &p, const PhotoId &phId) { // see Local::readPeer as well
	PhotoId newPhotoId = photoId;
	ImagePtr newPhoto = _userpic;
	StorageImageLocation newPhotoLoc = photoLoc;
	switch (p.type()) {
	case mtpc_chatPhoto: {
		const auto &d(p.c_chatPhoto());
		if (phId != UnknownPeerPhotoId) {
			newPhotoId = phId;
		}
		newPhotoLoc = App::imageLocation(160, 160, d.vphoto_small);
		newPhoto = newPhotoLoc.isNull() ? ImagePtr() : ImagePtr(newPhotoLoc);
//		photoFull = newPhoto ? ImagePtr(640, 640, d.vphoto_big, newPhoto) : ImagePtr();
	} break;
	default: {
		newPhotoId = 0;
		newPhotoLoc = StorageImageLocation();
		newPhoto = ImagePtr();
//		photoFull = ImagePtr();
	} break;
	}
	if (newPhotoId != photoId || newPhoto.v() != _userpic.v() || newPhotoLoc != photoLoc) {
		photoId = newPhotoId;
		setUserpic(newPhoto);
		photoLoc = newPhotoLoc;
		Notify::peerUpdatedDelayed(this, UpdateFlag::PhotoChanged);
	}
}

void ChannelData::setName(const QString &newName, const QString &newUsername) {
	updateNameDelayed(newName.isEmpty() ? name : newName, QString(), newUsername);
}

void PeerData::updateFull() {
	if (!_lastFullUpdate || getms(true) > _lastFullUpdate + kUpdateFullPeerTimeout) {
		updateFullForced();
	}
}

void PeerData::updateFullForced() {
	Auth().api().requestFullPeer(this);
	if (auto channel = asChannel()) {
		if (!channel->amCreator() && !channel->inviter) {
			Auth().api().requestSelfParticipant(channel);
		}
	}
}

void PeerData::fullUpdated() {
	_lastFullUpdate = getms(true);
}

bool ChannelData::setAbout(const QString &newAbout) {
	if (_about == newAbout) {
		return false;
	}
	_about = newAbout;
	Notify::peerUpdatedDelayed(this, UpdateFlag::AboutChanged);
	return true;
}

void ChannelData::setInviteLink(const QString &newInviteLink) {
	if (newInviteLink != _inviteLink) {
		_inviteLink = newInviteLink;
		Notify::peerUpdatedDelayed(this, UpdateFlag::InviteLinkChanged);
	}
}

void ChannelData::setMembersCount(int newMembersCount) {
	if (_membersCount != newMembersCount) {
		if (isMegagroup() && !mgInfo->lastParticipants.isEmpty()) {
			mgInfo->lastParticipantsStatus |= MegagroupInfo::LastParticipantsCountOutdated;
			mgInfo->lastParticipantsCount = membersCount();
		}
		_membersCount = newMembersCount;
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::MembersChanged);
	}
}

void ChannelData::setAdminsCount(int newAdminsCount) {
	if (_adminsCount != newAdminsCount) {
		_adminsCount = newAdminsCount;
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::AdminsChanged);
	}
}

void ChannelData::setRestrictedCount(int newRestrictedCount) {
	if (_restrictedCount != newRestrictedCount) {
		_restrictedCount = newRestrictedCount;
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::BannedUsersChanged);
	}
}

void ChannelData::setKickedCount(int newKickedCount) {
	if (_kickedCount != newKickedCount) {
		_kickedCount = newKickedCount;
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::BannedUsersChanged);
	}
}

MTPChannelBannedRights ChannelData::KickedRestrictedRights() {
	using Flag = MTPDchannelBannedRights::Flag;
	auto flags = Flag::f_view_messages | Flag::f_send_messages | Flag::f_send_media | Flag::f_embed_links | Flag::f_send_stickers | Flag::f_send_gifs | Flag::f_send_games | Flag::f_send_inline;
	return MTP_channelBannedRights(MTP_flags(flags), MTP_int(std::numeric_limits<int32>::max()));
}

void ChannelData::applyEditAdmin(not_null<UserData*> user, const MTPChannelAdminRights &oldRights, const MTPChannelAdminRights &newRights) {
	auto flags = Notify::PeerUpdate::Flag::AdminsChanged | Notify::PeerUpdate::Flag::None;
	if (mgInfo) {
		if (!mgInfo->lastParticipants.contains(user)) { // If rights are empty - still add participant? TODO check
			mgInfo->lastParticipants.push_front(user);
			setMembersCount(membersCount() + 1);
			if (user->botInfo && !mgInfo->bots.contains(user)) {
				mgInfo->bots.insert(user);
				if (mgInfo->botStatus != 0 && mgInfo->botStatus < 2) {
					mgInfo->botStatus = 2;
				}
			}
		}
		if (mgInfo->lastRestricted.contains(user)) { // If rights are empty - still remove restrictions? TODO check
			mgInfo->lastRestricted.remove(user);
			if (restrictedCount() > 0) {
				setRestrictedCount(restrictedCount() - 1);
			}
		}
		auto it = mgInfo->lastAdmins.find(user);
		if (newRights.c_channelAdminRights().vflags.v != 0) {
			auto lastAdmin = MegagroupInfo::Admin { newRights };
			lastAdmin.canEdit = true;
			if (it == mgInfo->lastAdmins.cend()) {
				mgInfo->lastAdmins.insert(user, lastAdmin);
				setAdminsCount(adminsCount() + 1);
			} else {
				it.value() = lastAdmin;
			}
		} else {
			if (it != mgInfo->lastAdmins.cend()) {
				mgInfo->lastAdmins.erase(it);
				if (adminsCount() > 0) {
					setAdminsCount(adminsCount() - 1);
				}
			}
		}
	}
	if (oldRights.c_channelAdminRights().vflags.v && !newRights.c_channelAdminRights().vflags.v) {
		// We removed an admin.
		if (adminsCount() > 1) {
			setAdminsCount(adminsCount() - 1);
		}
		if (!isMegagroup() && user->botInfo && membersCount() > 1) {
			// Removing bot admin removes it from channel.
			setMembersCount(membersCount() - 1);
		}
	} else if (!oldRights.c_channelAdminRights().vflags.v && newRights.c_channelAdminRights().vflags.v) {
		// We added an admin.
		setAdminsCount(adminsCount() + 1);
		updateFullForced();
	}
	Notify::peerUpdatedDelayed(this, flags);
}

void ChannelData::applyEditBanned(not_null<UserData*> user, const MTPChannelBannedRights &oldRights, const MTPChannelBannedRights &newRights) {
	auto flags = Notify::PeerUpdate::Flag::BannedUsersChanged | Notify::PeerUpdate::Flag::None;
	if (mgInfo) {
		if (mgInfo->lastAdmins.contains(user)) { // If rights are empty - still remove admin? TODO check
			mgInfo->lastAdmins.remove(user);
			if (adminsCount() > 1) {
				setAdminsCount(adminsCount() - 1);
			} else {
				flags |= Notify::PeerUpdate::Flag::AdminsChanged;
			}
		}
		auto isKicked = (newRights.c_channelBannedRights().vflags.v & MTPDchannelBannedRights::Flag::f_view_messages);
		auto isRestricted = !isKicked && (newRights.c_channelBannedRights().vflags.v != 0);
		auto it = mgInfo->lastRestricted.find(user);
		if (isRestricted) {
			if (it == mgInfo->lastRestricted.cend()) {
				mgInfo->lastRestricted.insert(user, MegagroupInfo::Restricted { newRights });
				setRestrictedCount(restrictedCount() + 1);
			} else {
				it->rights = newRights;
			}
		} else {
			if (it != mgInfo->lastRestricted.cend()) {
				mgInfo->lastRestricted.erase(it);
				if (restrictedCount() > 0) {
					setRestrictedCount(restrictedCount() - 1);
				}
			}
			if (isKicked) {
				auto i = mgInfo->lastParticipants.indexOf(user);
				if (i >= 0) {
					mgInfo->lastParticipants.removeAt(i);
				}
				if (membersCount() > 1) {
					setMembersCount(membersCount() - 1);
				} else {
					mgInfo->lastParticipantsStatus |= MegagroupInfo::LastParticipantsCountOutdated;
					mgInfo->lastParticipantsCount = 0;
				}
				setKickedCount(kickedCount() + 1);
				if (mgInfo->bots.contains(user)) {
					mgInfo->bots.remove(user);
					if (mgInfo->bots.isEmpty() && mgInfo->botStatus > 0) {
						mgInfo->botStatus = -1;
					}
				}
				flags |= Notify::PeerUpdate::Flag::MembersChanged;
				Auth().data().removeMegagroupParticipant(this, user);
			}
		}
	}
	Notify::peerUpdatedDelayed(this, flags);
}

void ChannelData::setRestrictionReason(const QString &text) {
	if (_restrictionReason != text) {
		_restrictionReason = text;
		Notify::peerUpdatedDelayed(this, Notify::PeerUpdate::Flag::RestrictionReasonChanged);
	}
}

bool ChannelData::canNotEditLastAdmin(not_null<UserData*> user) const {
	if (mgInfo) {
		auto i = mgInfo->lastAdmins.constFind(user);
		if (i != mgInfo->lastAdmins.cend()) {
			return !i->canEdit;
		}
		return (user == mgInfo->creator);
	}
	return false;
}

bool ChannelData::canEditAdmin(not_null<UserData*> user) const {
	if (user->isSelf()) {
		return false;
	} else if (amCreator()) {
		return true;
	} else if (canNotEditLastAdmin(user)) {
		return false;
	}
	return adminRights() & AdminRight::f_add_admins;
}

bool ChannelData::canRestrictUser(not_null<UserData*> user) const {
	if (user->isSelf()) {
		return false;
	} else if (amCreator()) {
		return true;
	} else if (canNotEditLastAdmin(user)) {
		return false;
	}
	return adminRights() & AdminRight::f_ban_users;
}

void ChannelData::setAdminRights(const MTPChannelAdminRights &rights) {
	if (rights.c_channelAdminRights().vflags.v == adminRights()) {
		return;
	}
	_adminRights.set(rights.c_channelAdminRights().vflags.v);
	if (isMegagroup()) {
		if (hasAdminRights()) {
			if (!amCreator()) {
				auto me = MegagroupInfo::Admin { rights };
				me.canEdit = false;
				mgInfo->lastAdmins.insert(App::self(), me);
			}
			mgInfo->lastRestricted.remove(App::self());
		} else {
			mgInfo->lastAdmins.remove(App::self());
		}
	}
	Notify::peerUpdatedDelayed(this, UpdateFlag::ChannelRightsChanged | UpdateFlag::AdminsChanged | UpdateFlag::BannedUsersChanged);
}

void ChannelData::setRestrictedRights(const MTPChannelBannedRights &rights) {
	if (rights.c_channelBannedRights().vflags.v == restrictions()
		&& rights.c_channelBannedRights().vuntil_date.v == _restrictedUntill) {
		return;
	}
	_restrictedUntill = rights.c_channelBannedRights().vuntil_date.v;
	_restrictions.set(rights.c_channelBannedRights().vflags.v);
	if (isMegagroup()) {
		if (hasRestrictions()) {
			if (!amCreator()) {
				auto me = MegagroupInfo::Restricted { rights };
				mgInfo->lastRestricted.insert(App::self(), me);
			}
			mgInfo->lastAdmins.remove(App::self());
		} else {
			mgInfo->lastRestricted.remove(App::self());
		}
	}
	Notify::peerUpdatedDelayed(this, UpdateFlag::ChannelRightsChanged | UpdateFlag::AdminsChanged | UpdateFlag::BannedUsersChanged);
}

uint64 PtsWaiter::ptsKey(PtsSkippedQueue queue, int32 pts) {
	return _queue.insert(uint64(uint32(pts)) << 32 | (++_skippedKey), queue).key();
}

void PtsWaiter::setWaitingForSkipped(ChannelData *channel, int32 ms) {
	if (ms >= 0) {
		if (App::main()) {
			App::main()->ptsWaiterStartTimerFor(channel, ms);
		}
		_waitingForSkipped = true;
	} else {
		_waitingForSkipped = false;
		checkForWaiting(channel);
	}
}

void PtsWaiter::setWaitingForShortPoll(ChannelData *channel, int32 ms) {
	if (ms >= 0) {
		if (App::main()) {
			App::main()->ptsWaiterStartTimerFor(channel, ms);
		}
		_waitingForShortPoll = true;
	} else {
		_waitingForShortPoll = false;
		checkForWaiting(channel);
	}
}

void PtsWaiter::checkForWaiting(ChannelData *channel) {
	if (!_waitingForSkipped && !_waitingForShortPoll && App::main()) {
		App::main()->ptsWaiterStartTimerFor(channel, -1);
	}
}

void PtsWaiter::applySkippedUpdates(ChannelData *channel) {
	if (!_waitingForSkipped) return;

	setWaitingForSkipped(channel, -1);

	if (_queue.isEmpty()) return;

	++_applySkippedLevel;
	for (auto i = _queue.cbegin(), e = _queue.cend(); i != e; ++i) {
		switch (i.value()) {
		case SkippedUpdate: Auth().api().applyUpdateNoPtsCheck(_updateQueue.value(i.key())); break;
		case SkippedUpdates: Auth().api().applyUpdatesNoPtsCheck(_updatesQueue.value(i.key())); break;
		}
	}
	--_applySkippedLevel;
	clearSkippedUpdates();
}

void PtsWaiter::clearSkippedUpdates() {
	_queue.clear();
	_updateQueue.clear();
	_updatesQueue.clear();
	_applySkippedLevel = 0;
}

bool PtsWaiter::updated(ChannelData *channel, int32 pts, int32 count, const MTPUpdates &updates) {
	if (_requesting || _applySkippedLevel) {
		return true;
	} else if (pts <= _good && count > 0) {
		return false;
	} else if (check(channel, pts, count)) {
		return true;
	}
	_updatesQueue.insert(ptsKey(SkippedUpdates, pts), updates);
	return false;
}

bool PtsWaiter::updated(ChannelData *channel, int32 pts, int32 count, const MTPUpdate &update) {
	if (_requesting || _applySkippedLevel) {
		return true;
	} else if (pts <= _good && count > 0) {
		return false;
	} else if (check(channel, pts, count)) {
		return true;
	}
	_updateQueue.insert(ptsKey(SkippedUpdate, pts), update);
	return false;
}

bool PtsWaiter::updated(ChannelData *channel, int32 pts, int32 count) {
	if (_requesting || _applySkippedLevel) {
		return true;
	} else if (pts <= _good && count > 0) {
		return false;
	}
	return check(channel, pts, count);
}

bool PtsWaiter::updateAndApply(ChannelData *channel, int32 pts, int32 count, const MTPUpdates &updates) {
	if (!updated(channel, pts, count, updates)) {
		return false;
	}
	if (!_waitingForSkipped || _queue.isEmpty()) {
		// Optimization - no need to put in queue and back.
		Auth().api().applyUpdatesNoPtsCheck(updates);
	} else {
		_updatesQueue.insert(ptsKey(SkippedUpdates, pts), updates);
		applySkippedUpdates(channel);
	}
	return true;
}

bool PtsWaiter::updateAndApply(ChannelData *channel, int32 pts, int32 count, const MTPUpdate &update) {
	if (!updated(channel, pts, count, update)) {
		return false;
	}
	if (!_waitingForSkipped || _queue.isEmpty()) {
		// Optimization - no need to put in queue and back.
		Auth().api().applyUpdateNoPtsCheck(update);
	} else {
		_updateQueue.insert(ptsKey(SkippedUpdate, pts), update);
		applySkippedUpdates(channel);
	}
	return true;
}

bool PtsWaiter::updateAndApply(ChannelData *channel, int32 pts, int32 count) {
	if (!updated(channel, pts, count)) {
		return false;
	}
	applySkippedUpdates(channel);
	return true;
}

bool PtsWaiter::check(ChannelData *channel, int32 pts, int32 count) { // return false if need to save that update and apply later
	if (!inited()) {
		init(pts);
		return true;
	}

	_last = qMax(_last, pts);
	_count += count;
	if (_last == _count) {
		_good = _last;
		return true;
	} else if (_last < _count) {
		setWaitingForSkipped(channel, 1);
	} else {
		setWaitingForSkipped(channel, WaitForSkippedTimeout);
	}
	return !count;
}
