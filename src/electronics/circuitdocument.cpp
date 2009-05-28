/***************************************************************************
 *   Copyright (C) 2003-2005 by David Saxton                               *
 *   david@bluehaze.org                                                    *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 ***************************************************************************/

#include "canvasmanipulator.h"
#include "circuitdocument.h"
#include "circuiticndocument.h"
#include "circuitview.h"
#include "component.h"
#include "connector.h"
#include "src/core/ktlconfig.h"
#include "cnitemgroup.h"
#include "documentiface.h"
#include "drawpart.h"
#include "ecnode.h"
#include "itemdocumentdata.h"
#include "ktechlab.h"
#include "pin.h"
#include "simulator.h"
#include "subcircuits.h"
#include "switch.h"

#include <kdebug.h>
#include <kinputdialog.h>
#include <klocale.h>
#include <kmessagebox.h>
#include <qregexp.h>
#include <qtimer.h>

CircuitDocument::CircuitDocument(const QString &caption, const char *name)
		: CircuitICNDocument(caption, name) {
	m_pOrientationAction = new KActionMenu(i18n("Orientation"), "rotate", this);

	m_type = Document::dt_circuit;
	m_pDocumentIface = new CircuitDocumentIface(this);
	m_fileExtensionInfo = QString("*.circuit|%1(*.circuit)\n*|%2").arg(i18n("Circuit")).arg(i18n("All Files"));

	m_cmManager->addManipulatorInfo(CMSelect::manipulatorInfo());
	m_cmManager->addManipulatorInfo(CMDraw::manipulatorInfo());
	m_cmManager->addManipulatorInfo(CMRightClick::manipulatorInfo());
	m_cmManager->addManipulatorInfo(CMRepeatedItemAdd::manipulatorInfo());
	m_cmManager->addManipulatorInfo(CMItemResize::manipulatorInfo());
	m_cmManager->addManipulatorInfo(CMItemDrag::manipulatorInfo());

	connect(this, SIGNAL(connectorAdded(Connector*)), this, SLOT(requestAssignCircuits()));
	connect(this, SIGNAL(connectorAdded(Connector*)), this, SLOT(connectorAdded(Connector*)));

	m_updateCircuitsTmr = new QTimer();
	connect(m_updateCircuitsTmr, SIGNAL(timeout()), this, SLOT(assignCircuits()));

	requestStateSave();
}

CircuitDocument::~CircuitDocument() {
	m_bDeleted = true;
	deleteCircuits();

	delete m_updateCircuitsTmr;
	delete m_pDocumentIface;
}

void CircuitDocument::slotInitItemActions() {
	CircuitICNDocument::slotInitItemActions();

	CircuitView *activeCircuitView = dynamic_cast<CircuitView*>(activeView());
	if (!KTechlab::self() || !activeCircuitView) return;

	Component *item = dynamic_cast<Component*>(m_selectList->activeItem());
	if ((!item && m_selectList->count() > 0) || !m_selectList->itemsAreSameType())
		return;

	KAction * orientation_actions[] = {
		activeCircuitView->action("edit_orientation_0"),
		activeCircuitView->action("edit_orientation_90"),
		activeCircuitView->action("edit_orientation_180"),
		activeCircuitView->action("edit_orientation_270")
	};

	if (!item) {
		for (unsigned i = 0; i < 4; ++i)
			orientation_actions[i]->setEnabled(false);

		return;
	}

	for (unsigned i = 0; i < 4; ++ i) {
		orientation_actions[i]->setEnabled(true);
		m_pOrientationAction->remove(orientation_actions[i]);
		m_pOrientationAction->insert(orientation_actions[i]);
	}

	if (item->angleDegrees() == 0)
		(static_cast<KToggleAction*>(orientation_actions[0]))->setChecked(true);
	else if (item->angleDegrees() == 90)
		(static_cast<KToggleAction*>(orientation_actions[1]))->setChecked(true);
	else if (item->angleDegrees() == 180)
		(static_cast<KToggleAction*>(orientation_actions[2]))->setChecked(true);
	else if (item->angleDegrees() == 270)
		(static_cast<KToggleAction*>(orientation_actions[3]))->setChecked(true);
}

void CircuitDocument::rotateCounterClockwise() {
	m_selectList->slotRotateCCW();
	requestRerouteInvalidatedConnectors();
}

void CircuitDocument::rotateClockwise() {
	m_selectList->slotRotateCW();
	requestRerouteInvalidatedConnectors();
}

void CircuitDocument::flipHorizontally() {
	m_selectList->flipHorizontally();
	requestRerouteInvalidatedConnectors();
}

void CircuitDocument::flipVertically() {
	m_selectList->flipVertically();
	requestRerouteInvalidatedConnectors();
}

void CircuitDocument::setOrientation0() {
	m_selectList->slotSetOrientation0();
	requestRerouteInvalidatedConnectors();
}

void CircuitDocument::setOrientation90() {
	m_selectList->slotSetOrientation90();
	requestRerouteInvalidatedConnectors();
}

void CircuitDocument::setOrientation180() {
	m_selectList->slotSetOrientation180();
	requestRerouteInvalidatedConnectors();
}

void CircuitDocument::setOrientation270() {
	m_selectList->slotSetOrientation270();
	requestRerouteInvalidatedConnectors();
}

View *CircuitDocument::createView(ViewContainer *viewContainer, uint viewAreaId, const char *name) {
	View *view = new CircuitView(this, viewContainer, viewAreaId, name);
	handleNewView(view);
	return view;
}

void CircuitDocument::slotUpdateConfiguration() {
	CircuitICNDocument::slotUpdateConfiguration();

	ECNodeMap::iterator nodeEnd = m_ecNodeList.end();
	for (ECNodeMap::iterator it = m_ecNodeList.begin(); it != nodeEnd; ++it) {
		ECNode *n = it->second;
		n->setShowVoltageBars(KTLConfig::showVoltageBars());
		n->setShowVoltageColor(KTLConfig::showVoltageColor());
	}

	ConnectorList::iterator connectorsEnd = m_connectorList.end();
	for (ConnectorList::iterator it = m_connectorList.begin(); it != connectorsEnd; ++it)
		(*it)->updateConnectorLines();

	ComponentList::iterator componentsEnd = m_componentList.end();
	for (ComponentList::iterator it = m_componentList.begin(); it != componentsEnd; ++it)
		(*it)->slotUpdateConfiguration();
}

void CircuitDocument::update() {
	CircuitICNDocument::update();

	bool animWires = KTLConfig::animateWires();
	if (KTLConfig::showVoltageColor() || animWires) {
		if (animWires) {
			// Wire animation is for showing currents, so we need to recalculate the currents
			// in the wires.
			calculateConnectorCurrents();
		}

		ConnectorList::iterator end = m_connectorList.end();
		for (ConnectorList::iterator it = m_connectorList.begin(); it != end; ++it) {
			(*it)->incrementCurrentAnimation(1.0 / double(KTLConfig::refreshRate()));
			(*it)->updateConnectorLines(animWires);
		}
	}

	if (KTLConfig::showVoltageColor() || KTLConfig::showVoltageBars()) {

	{// we should *NOT* be seeing zeros here, but we have to remove them anyway.
// FIXME; workaround code!! 
		ECNodeMap::iterator it = m_ecNodeList.begin();
		const ECNodeMap::iterator nlEnd = m_ecNodeList.end();
		while(it != nlEnd) {
			if(!it->second) { 
				ECNodeMap::iterator dud = it; 
				it++;
				m_ecNodeList.erase(dud);
			} else it++;
	}}

		ECNodeMap::iterator end = m_ecNodeList.end();
		for (ECNodeMap::iterator it = m_ecNodeList.begin(); it != end; ++it) {
			it->second->setNodeChanged();
		}
	}
}

void CircuitDocument::fillContextMenu(const QPoint &pos) {
	CircuitICNDocument::fillContextMenu(pos);
	CircuitView *activeCircuitView = dynamic_cast<CircuitView*>(activeView());

	if (!activeCircuitView) return;

	bool canCreateSubcircuit = (m_selectList->count() > 1 && countExtCon(m_selectList->items()) > 0);
	KAction *subcircuitAction = activeCircuitView->action("circuit_create_subcircuit");
	subcircuitAction->setEnabled(canCreateSubcircuit);

	if (m_selectList->count() < 1) return;

	Component *item = dynamic_cast<Component*>(selectList()->activeItem());

	if (item || (m_selectList->count() > 1 && m_selectList->itemsAreSameType())) {
		KAction *orientation_actions[] = {
			activeCircuitView->action("edit_orientation_0"),
			activeCircuitView->action("edit_orientation_90"),
			activeCircuitView->action("edit_orientation_180"),
			activeCircuitView->action("edit_orientation_270")
		};

		for (unsigned i = 0; i < 4; ++ i) {
			m_pOrientationAction->remove(orientation_actions[i]);
			m_pOrientationAction->insert(orientation_actions[i]);
		}

		QPtrList<KAction> orientation_actionlist;

		// 	orientation_actionlist.prepend( new KActionSeparator() );
		orientation_actionlist.append(m_pOrientationAction);
		KTechlab::self()->plugActionList("orientation_actionlist", orientation_actionlist);
	}
}

void CircuitDocument::deleteCircuits() {
	const CircuitList::iterator end = m_circuitList.end();
	for (CircuitList::iterator it = m_circuitList.begin(); it != end; ++it) {
		Simulator::self()->detachCircuit(*it);
		delete *it;
	}

	m_circuitList.clear();
	m_wireList.clear();
}

void CircuitDocument::requestAssignCircuits() {
// 	kdDebug() << k_funcinfo << endl;
	deleteCircuits();
	m_updateCircuitsTmr->stop();
	m_updateCircuitsTmr->start(0, true);
}

void CircuitDocument::connectorAdded(Connector * connector) {
	if (connector) {
//		connect(connector, SIGNAL(numWiresChanged(unsigned)), this, SLOT(requestAssignCircuits()));
		connect(connector, SIGNAL(removed(Connector*)), this, SLOT(requestAssignCircuits()));
	}
}

void CircuitDocument::itemAdded(Item * item) {
	CircuitICNDocument::itemAdded(item);
	componentAdded(item);
}

void CircuitDocument::componentAdded(Item *item) {
	Component *component = dynamic_cast<Component*>(item);

	if (!component) return;

	requestAssignCircuits();

	connect(component, SIGNAL(elementCreated(Element*)), this, SLOT(requestAssignCircuits()));
	connect(component, SIGNAL(elementDestroyed(Element*)), this, SLOT(requestAssignCircuits()));
	connect(component, SIGNAL(removed(Item*)), this, SLOT(componentRemoved(Item*)));

	// We don't attach the component to the Simulator just yet, as the
	// Component's vtable is not yet fully constructed, and so Simulator can't
	// tell whether or not it is a logic component
	m_toSimulateList.insert(component);
}

void CircuitDocument::componentRemoved(Item *item) {
	Component *component = dynamic_cast<Component*>(item);

	if (!component) return;

	m_componentList.erase(component);
	m_toSimulateList.erase(component);
	requestAssignCircuits();
	Simulator::self()->detachComponent(component);
}

/// I think this is where the inf0z from cnodes/branches is moved into the midle-layer
/// pins/wires.
void CircuitDocument::calculateConnectorCurrents() {

	PinSet m_pinList;

	const CircuitList::iterator circuitEnd = m_circuitList.end();
	for (CircuitList::iterator it = m_circuitList.begin(); it != circuitEnd; ++it) {
		(*it)->updateCurrents();
		PinSet *foo = (*it)->getPins();
		m_pinList.insert(foo->begin(), foo->end());
	}

	PinSet groundPins;

	// Tell the Pins to reset their calculated currents to zero
	const PinSet::iterator pinEnd = m_pinList.end();
	for (PinSet::iterator it = m_pinList.begin(); it != pinEnd; ++it) {
		Pin *n = *it;
		n->resetCurrent();
		n->setSwitchCurrentsUnknown();

		if (!n->parentECNode()->isChildNode()) {
			n->setCurrentKnown(true);
			// (and it has a current of 0 amps)
		} else if (n->groundType() == Pin::gt_always) {
			groundPins.insert(n);
			n->setCurrentKnown(false);
		} else {
			// Child node that is non ground
			n->setCurrentKnown(n->parentECNode()->numPins() < 2);
		}
	}

	// Tell the components to update their ECNode's currents' from the elements
// currents are merged into PINS.
	const ComponentList::iterator componentEnd = m_componentList.end();
	for (ComponentList::iterator it = m_componentList.begin(); it != componentEnd; ++it)
		(*it)->setNodalCurrents();

	// And now for the wires and switches...
	const WireList::iterator clEnd = m_wireList.end();
	for (WireList::iterator it = m_wireList.begin(); it != clEnd; ++it)
		(*it)->setCurrentKnown(false);

	SwitchList switches = m_switchList;
	WireList wires = m_wireList;
	bool found = true;
	while ((!wires.empty() || !switches.empty() || !groundPins.empty()) && found) {
		found = false;

		WireList::iterator wiresEnd = wires.end();
		for (WireList::iterator it = wires.begin(); it != wiresEnd;) {
			if ((*it)->calculateCurrent()) {
				found = true;
				WireList::iterator oldIt = it;
				++it;
				wires.erase(oldIt);
			} else	++it;
		}

		SwitchList::iterator switchesEnd = switches.end();
		for (SwitchList::iterator it = switches.begin(); it != switchesEnd;) {
			if ((*it)->calculateCurrent()) {
				found = true;
				SwitchList::iterator oldIt = it;
				++it;
				switches.remove(oldIt);
			} else ++it;
		}

		/*
		make the ground pins work. Current engine doesn't treat ground explicitly.
		*/
		PinSet::iterator groundPinsEnd = groundPins.end();
		for (PinSet::iterator it = groundPins.begin(); it != groundPinsEnd;) {
			if ((*it)->calculateCurrentFromWires()) {
				found = true;
				PinSet::iterator oldIt = it;
				++it;
				groundPins.erase(oldIt);
			} else ++it;
		}
	}
}

void CircuitDocument::assignCircuits() {
	// Now we can finally add the unadded components to the Simulator
	const ComponentList::iterator toSimulateEnd = m_toSimulateList.end();

	for (ComponentList::iterator it = m_toSimulateList.begin(); it != toSimulateEnd; ++it)
		Simulator::self()->attachComponent(*it);

	m_toSimulateList.clear();

	// Stage 0: Build up pin and wire lists
	PinSet unassignedPins;


	{// we should *NOT* be seeing zeros here, but we have to remove them anyway.
// FIXME; workaround code!! 
		ECNodeMap::iterator it = m_ecNodeList.begin();
		const ECNodeMap::iterator nlEnd = m_ecNodeList.end();
		while(it != nlEnd) {
			if(!it->second) { 
				ECNodeMap::iterator dud = it; 
				it++;
				m_ecNodeList.erase(dud);
			} else it++;
	}}

	const ECNodeMap::const_iterator nodeListEnd = m_ecNodeList.end();
	for (ECNodeMap::const_iterator it = m_ecNodeList.begin(); it != nodeListEnd; ++it) {
		ECNode *ecnode = it->second;
//		assert(ecnode);

		for (unsigned i = 0; i < ecnode->numPins(); i++) {
			Pin *foo = ecnode->pin(i);
			assert(foo);
			unassignedPins.insert(foo);
		}
	}

	m_wireList.clear();

	const ConnectorList::const_iterator connectorListEnd = m_connectorList.end();
	for (ConnectorList::const_iterator it = m_connectorList.begin(); it != connectorListEnd; ++it) {
		for (unsigned i = 0; i < (*it)->numWires(); i++)
			m_wireList.insert((*it)->wire(i));
	}

	typedef QValueList<PinSet> PinSetList;

	// Stage 1: Partition the circuit up into dependent areas (bar splitting
	// at ground pins)
	PinSetList pinListList;

	while (!unassignedPins.empty()) {
		PinSet pinList;
		getPartition(*unassignedPins.begin(), &pinList, &unassignedPins);
		pinListList.append(pinList);
	}

// 	kdDebug () << "pinListList.size()="<<pinListList.size()<<endl;

	// Stage 2: Split up each partition into circuits by ground pins
	const PinSetList::iterator nllEnd = pinListList.end();
	for (PinSetList::iterator it = pinListList.begin(); it != nllEnd; ++it)
		splitIntoCircuits(&*it);

	// Stage 3: Initialize the circuits
	const CircuitList::iterator circuitListEnd = m_circuitList.end();
	for (CircuitList::iterator it = m_circuitList.begin(); it != circuitListEnd; ++it)
		(*it)->init();

	m_switchList.clear();
	m_componentList.clear();

	const ItemMap::const_iterator cilEnd = m_itemList.end();
	for (ItemMap::const_iterator it = m_itemList.begin(); it != cilEnd; ++it) {
		Component *component = dynamic_cast<Component*>(it->second);

		if (!component) continue;

		m_componentList.insert(component);
		component->initNodes();

		m_switchList += component->switchList();
	}

	for (CircuitList::iterator it = m_circuitList.begin(); it != circuitListEnd; ++it) {
		(*it)->initCache();
		Simulator::self()->attachCircuit(*it);
	}
}

void CircuitDocument::getPartition(Pin *pin, PinSet *pinList, PinSet *unassignedPins, bool onlyGroundDependent) {
	if (!pin) return;

assert(pin != (Pin *)0x40);

	unassignedPins->erase(pin);

	if(!pinList->insert(pin).second) return;

	const PinSet localConnectedPins = pin->localConnectedPins();
	const PinSet::const_iterator end = localConnectedPins.end();
	for (PinSet::const_iterator it = localConnectedPins.begin(); it != end; ++it)
		getPartition(*it, pinList, unassignedPins, onlyGroundDependent);

	const PinSet groundDependentPins = pin->groundDependentPins();
	const PinSet::const_iterator dEnd = groundDependentPins.end();
	for (PinSet::const_iterator it = groundDependentPins.begin(); it != dEnd; ++it)
		getPartition(*it, pinList, unassignedPins, onlyGroundDependent);

	if (!onlyGroundDependent) {
		PinSet circuitDependentPins = pin->circuitDependentPins();
		const PinSet::const_iterator dEnd = circuitDependentPins.end();

		for (PinSet::const_iterator it = circuitDependentPins.begin(); it != dEnd; ++it)
			getPartition(*it, pinList, unassignedPins, onlyGroundDependent);
	}
}

void CircuitDocument::splitIntoCircuits(PinSet *pinList) {
	// First: identify ground
	PinSet unassignedPins = *pinList;
	typedef QValueList<PinSet> PinSetList;
	PinSetList pinListList;

	while (!unassignedPins.empty()) {
		PinSet tempPinSet;
		getPartition(*unassignedPins.begin(), &tempPinSet, &unassignedPins, true);
		pinListList.append(tempPinSet);
	}

	const PinSetList::iterator nllEnd = pinListList.end();
	for (PinSetList::iterator it = pinListList.begin(); it != nllEnd; ++it)
		Circuit::identifyGround(*it);

	while (!pinList->empty()) {
		PinSet::iterator end = pinList->end();
		PinSet::iterator it = pinList->begin();

		while (it != end && (*it)->eqId() == -1)
			++it;

		if (it == end) break;
		else {
			Circuitoid *circuitoid = new Circuitoid();
			recursivePinAdd(*it, circuitoid, pinList);

// BUG WORKAROUND; FIXME recursivePinAdd!!!
			if(circuitoid->getElementsBegin() != circuitoid->getElementsEnd() && !tryAsLogicCircuit(circuitoid)) { 
				m_circuitList.insert(createCircuit(circuitoid));
			}

			delete circuitoid;
		}
	}

	// Remaining pins are ground; tell them about it
	// TODO This is a bit hacky....
	const PinSet::iterator end = pinList->end();
	for (PinSet::iterator it = pinList->begin(); it != end; ++it) {
		(*it)->setVoltage(0.0);

		ElementList elements = (*it)->elements();
		const ElementList::iterator eEnd = elements.end();
		for (ElementList::iterator it = elements.begin(); it != eEnd; ++it) {
			if (LogicIn *logicIn = dynamic_cast<LogicIn*>(*it)) {
				logicIn->setLastState(false);
				logicIn->callCallback();
			}
		}
	}
}

void CircuitDocument::recursivePinAdd(Pin *pin, Circuitoid *circuitoid, PinSet *unassignedPins) {
	if (!pin) return;

	if (pin->eqId() != -1)
		unassignedPins->erase(pin);

	if (!circuitoid->addPin(pin)) return;

	if (pin->eqId() == -1) return;

	{
		const PinSet localConnectedPins = pin->localConnectedPins();
		const PinSet::const_iterator end = localConnectedPins.end();
		for (PinSet::const_iterator it = localConnectedPins.begin(); it != end; ++it)
			recursivePinAdd(*it, circuitoid, unassignedPins);
	}

	{
		const PinSet groundDependentPins = pin->groundDependentPins();
		const PinSet::const_iterator gdEnd = groundDependentPins.end();
		for (PinSet::const_iterator it = groundDependentPins.begin(); it != gdEnd; ++it)
			recursivePinAdd(*it, circuitoid, unassignedPins);
	}
	{
		const PinSet circuitDependentPins = pin->circuitDependentPins();
		const PinSet::const_iterator cdEnd = circuitDependentPins.end();
		for (PinSet::const_iterator it = circuitDependentPins.begin(); it != cdEnd; ++it)
			recursivePinAdd(*it, circuitoid, unassignedPins);
	}
	{
		const ElementList elements = pin->elements();
		const ElementList::const_iterator eEnd = elements.end();
		for (ElementList::const_iterator it = elements.begin(); it != eEnd; ++it)
			circuitoid->addElement(*it);
	}
}

bool CircuitDocument::tryAsLogicCircuit(Circuitoid *circuitoid) {
	if (!circuitoid) return false;

	if (circuitoid->numElements() == 0) {
		// This doesn't quite belong here...but whatever. Initialize all
		// pins to voltage zero as they won't get set to zero otherwise
		const PinSet::const_iterator pinListEnd = circuitoid->getPinsEnd();
		for (PinSet::const_iterator it = circuitoid->getPinsBegin(); it != pinListEnd; ++it)
			(*it)->setVoltage(0.0);

		// A logic circuit only requires there to be no non-logic components,
		// and at most one LogicOut - so this qualifies
		return true;
	}

	LogicInList logicInList;
	LogicOut *out = 0;
	uint logicInCount = 0;
	uint logicOutCount = 0;

	ElementList::const_iterator end = circuitoid->getElementsEnd();
	for (ElementList::const_iterator it = circuitoid->getElementsBegin(); it != end; ++it) {
		if ((*it)->type() == Element::Element_LogicOut) {
			logicOutCount++;
			out = static_cast<LogicOut*>(*it);
		} else if ((*it)->type() == Element::Element_LogicIn) {
			logicInCount++;
			logicInList += static_cast<LogicIn*>(*it);
		} else return false;
	}

	if (logicOutCount > 1) return false;
	else if (logicOutCount == 1) {
		Simulator::self()->createLogicChain(out, logicInList);
		out->pinList = circuitoid->getPinSet();
	} else {
		// We have ourselves stranded LogicIns...so lets set them all to low

		const PinSet::const_iterator pinListEnd = circuitoid->getPinsEnd();
		for (PinSet::const_iterator it = circuitoid->getPinsBegin(); it != pinListEnd; ++it)
			(*it)->setVoltage(0.0);

		for (ElementList::const_iterator it = circuitoid->getElementsBegin(); it != end; ++it) {
			LogicIn *logicIn = static_cast<LogicIn*>(*it);
			logicIn->setNextLogic(0);
			logicIn->setElementSet(0);

			if (logicIn->isHigh()) {
				logicIn->setLastState(false);
				logicIn->callCallback();
			}
		}
	}

	return true;
}

Circuit *CircuitDocument::createCircuit(Circuitoid *circuitoid) {
	if (!circuitoid) return 0;

	Circuit *circuit = new Circuit();

	const PinSet::const_iterator nEnd = circuitoid->getPinsEnd();
	for (PinSet::const_iterator it = circuitoid->getPinsBegin(); it != nEnd; ++it)
		circuit->addPin(*it);

	const ElementList::const_iterator eEnd = circuitoid->getElementsEnd();
	for (ElementList::const_iterator it = circuitoid->getElementsBegin(); it != eEnd; ++it)
		circuit->addElement(*it);

	return circuit;
}

void CircuitDocument::createSubcircuit() {
	ItemList itemList = m_selectList->items();
	const ItemList::iterator itemListEnd = itemList.end();
	for (ItemList::iterator it = itemList.begin(); it != itemListEnd; ++it) {
		if (!dynamic_cast<Component*>((Item*)*it))
			*it = 0;
	}

	itemList.remove((Item*)0);
	if (itemList.isEmpty()) {
		KMessageBox::sorry(activeView(), i18n("No components were found in the selection."));
		return;
	}

	// Number of external connections
	const int extConCount = countExtCon(itemList);
	if (extConCount == 0) {
		KMessageBox::sorry(activeView(), i18n("No External Connection components were found in the selection."));
		return;
	}

	bool ok;
	const QString name = KInputDialog::getText("Subcircuit", "Name", QString::null, &ok, activeView());

	if (!ok) return;

	SubcircuitData subcircuit;
	subcircuit.addItems(itemList);
	subcircuit.addNodes(getCommonNodes(itemList));
	subcircuit.addConnectors(getCommonConnectors(itemList));
	Subcircuits::addSubcircuit(name, subcircuit.toXML());
}

int CircuitDocument::countExtCon(const ItemList &itemList) const {
	int count = 0;
	const ItemList::const_iterator end = itemList.end();

	for (ItemList::const_iterator it = itemList.begin(); it != end; ++it) {
		Item *item = *it;

		if (item && item->type() == "ec/external_connection")
			count++;
	}

	return count;
}

bool CircuitDocument::isValidItem(const QString &itemId) {
	return itemId.startsWith("ec/") || itemId.startsWith("dp/") || itemId.startsWith("sc/");
}

bool CircuitDocument::isValidItem(Item *item) {
	return (dynamic_cast<Component*>(item) || dynamic_cast<DrawPart*>(item));
}

void CircuitDocument::displayEquations() {
	kdDebug() << "######################################################" << endl;
	const CircuitList::iterator end = m_circuitList.end();
	int i = 1;

	for (CircuitList::iterator it = m_circuitList.begin(); it != end; ++it) {
		kdDebug() << "Equation set " << i << ":\n";
		(*it)->displayEquations();
		i++;
	}

	kdDebug() << "######################################################" << endl;
}

#include "circuitdocument.moc"
