function Heal() {}

Heal.prototype.Schema =
	"<a:help>Controls the healing abilities of the unit.</a:help>" +
	"<a:example>" +
		"<Range>20</Range>" +
		"<RangeOverlay>" +
			"<LineTexture>heal_overlay_range.png</LineTexture>" +
			"<LineTextureMask>heal_overlay_range_mask.png</LineTextureMask>" +
			"<LineThickness>0.35</LineThickness>" +
		"</RangeOverlay>" +
		"<Health>5</Health>" +
		"<Interval>2000</Interval>" +
		"<UnhealableClasses datatype=\"tokens\">Cavalry</UnhealableClasses>" +
		"<HealableClasses datatype=\"tokens\">Support Infantry</HealableClasses>" +
	"</a:example>" +
	"<element name='Range' a:help='Range (in metres) where healing is possible.'>" +
		"<ref name='nonNegativeDecimal'/>" +
	"</element>" +
	"<optional>" +
		"<element name='RangeOverlay'>" +
			"<interleave>" +
				"<element name='LineTexture'><text/></element>" +
				"<element name='LineTextureMask'><text/></element>" +
				"<element name='LineThickness'><ref name='nonNegativeDecimal'/></element>" +
			"</interleave>" +
		"</element>" +
	"</optional>" +
	"<element name='Health' a:help='Health healed per Interval.'>" +
		"<ref name='nonNegativeDecimal'/>" +
	"</element>" +
	"<element name='Interval' a:help='A heal is performed every Interval ms.'>" +
		"<ref name='nonNegativeDecimal'/>" +
	"</element>" +
	"<element name='UnhealableClasses' a:help='If the target has any of these classes it can not be healed (even if it has a class from HealableClasses).'>" +
		"<attribute name='datatype'>" +
			"<value>tokens</value>" +
		"</attribute>" +
		"<text/>" +
	"</element>" +
	"<element name='HealableClasses' a:help='The target must have one of these classes to be healable.'>" +
		"<attribute name='datatype'>" +
			"<value>tokens</value>" +
		"</attribute>" +
		"<text/>" +
	"</element>";

Heal.prototype.Init = function()
{
};

// We have no dynamic state to save.
Heal.prototype.Serialize = null;

Heal.prototype.GetTimers = function()
{
	return {
		"prepare": 1000,
		"repeat": this.GetInterval()
	};
};

Heal.prototype.GetHealth = function()
{
	return ApplyValueModificationsToEntity("Heal/Health", +this.template.Health, this.entity);
};

Heal.prototype.GetInterval = function()
{
	return ApplyValueModificationsToEntity("Heal/Interval", +this.template.Interval, this.entity);
};

Heal.prototype.GetRange = function()
{
	return {
		"min": 0,
		"max": ApplyValueModificationsToEntity("Heal/Range", +this.template.Range, this.entity)
	};
};

Heal.prototype.GetUnhealableClasses = function()
{
	return this.template.UnhealableClasses._string || "";
};

Heal.prototype.GetHealableClasses = function()
{
	return this.template.HealableClasses._string || "";
};

/**
 * Whether this entity can heal the target.
 *
 * @param {number} target - The target's entity ID.
 * @return {boolean} - Whether the target can be healed.
 */
Heal.prototype.CanHeal = function(target)
{
	let cmpHealth = Engine.QueryInterface(target, IID_Health);
	if (!cmpHealth || cmpHealth.IsUnhealable())
		return false;

	// Verify that the target is owned by an ally or the player self.
	let cmpOwnership = Engine.QueryInterface(this.entity, IID_Ownership);
	if (!cmpOwnership || !IsOwnedByAllyOfPlayer(cmpOwnership.GetOwner(), target))
		return false;

	// Verify that the target has the right class.
	let cmpIdentity = Engine.QueryInterface(target, IID_Identity);
	if (!cmpIdentity)
		return false;

	let targetClasses = cmpIdentity.GetClassesList();
	return !MatchesClassList(targetClasses, this.GetUnhealableClasses()) &&
		MatchesClassList(targetClasses, this.GetHealableClasses());
};

Heal.prototype.GetRangeOverlays = function()
{
	if (!this.template.RangeOverlay)
		return [];

	return [{
		"radius": this.GetRange().max,
		"texture": this.template.RangeOverlay.LineTexture,
		"textureMask": this.template.RangeOverlay.LineTextureMask,
		"thickness": +this.template.RangeOverlay.LineThickness
	}];
};

/**
 * Heal the target entity. This should only be called after a successful range
 * check, and should only be called after GetTimers().repeat msec has passed
 * since the last call to PerformHeal.
 */
Heal.prototype.PerformHeal = function(target)
{
	let cmpHealth = Engine.QueryInterface(target, IID_Health);
	if (!cmpHealth)
		return;

	let targetState = cmpHealth.Increase(this.GetHealth());

	// Add experience.
	let cmpLoot = Engine.QueryInterface(target, IID_Loot);
	let cmpPromotion = Engine.QueryInterface(this.entity, IID_Promotion);
	if (targetState !== undefined && cmpLoot && cmpPromotion)
	{
		// Health healed times experience per health.
		cmpPromotion.IncreaseXp((targetState.new - targetState.old) / cmpHealth.GetMaxHitpoints() * cmpLoot.GetXp());
	}
	// TODO we need a sound file
//	PlaySound("heal_impact", this.entity);
};

Heal.prototype.OnValueModification = function(msg)
{
	if (msg.component != "Heal" || msg.valueNames.indexOf("Heal/Range") === -1)
		return;

	let cmpUnitAI = Engine.QueryInterface(this.entity, IID_UnitAI);
	if (!cmpUnitAI)
		return;

	cmpUnitAI.UpdateRangeQueries();
};

Engine.RegisterComponentType(IID_Heal, "Heal", Heal);
